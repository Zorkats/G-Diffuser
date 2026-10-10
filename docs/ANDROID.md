# Android

G-Diffuser runs on Android as a native app: the same decompiled game, the same libultraship
runtime, the same Fast3D renderer, compiled for arm64 and rendered through OpenGL ES 3.0. It is
not an emulator, and it is not a remote-play client.

Everything below is about the things that are genuinely different on a phone. Where a subject is
unchanged — enhancements, the menu, texture packs, what the game actually does — the
[README](../README.md) is still the reference.

---

## What you need

The same three files the desktop build needs, because the port is built Expansion-Kit-only:

| File | What it is |
| --- | --- |
| An F-Zero X **US rev0** ROM | Big-endian `.z64`. A `.n64`/`.v64` dump is rejected rather than byte-swapped. |
| `baserom.translated.ek.ndd` | The fan-translated Expansion Kit disk image (~64.9 MB). |
| `N64DDIPLROM.n64` | A 64DD IPL / drive ROM dump (~4 MB). The US prototype dump is accepted too. |

Nothing is bundled with the APK. You supply your own dumps, they stay on the device, and the
archives are built there.

A device needs **arm64** (which is every Android device sold since about 2019 — Google Play has
required 64-bit since then), **Android 7.0 or newer**, and **OpenGL ES 3.0**.

Leave about **1 GB free**. Your three dumps come to roughly 85 MB, the archives built from them
add a similar amount, and the extractor refuses to start without 384 MB free on top — the same
gate the desktop build applies, because it stages the whole archive before installing it.

## First run

The launcher asks for the three files in turn. Each one is copied into the app's own storage,
then judged by the port's own validators — the same code the desktop setup screen uses, reached
through JNI — so a row turns green only when the file is genuinely usable. An unrecognised but
structurally valid dump is accepted with a warning that stays on screen rather than silently.

With all three accepted, **Build the game data** runs the extractor. That takes a few minutes on
a mid-range phone and happens once. Keep the screen on; the launcher holds a wake lock, but
being killed part way through wastes the wait.

After that the launcher goes straight to the game on every later launch.

## Where files live

Everything is in the app's external files directory, which a file manager can reach:

```
Android/data/com.gdiffuser.android/files/
  baserom.us.rev0.z64          your cartridge ROM
  baserom.translated.ek.ndd    your Expansion Kit disk image
  N64DDIPLROM.n64              your 64DD IPL ROM
  fzerox.o2r                   built on device from the ROM
  fzerox-disk.o2r              built on device from the disk image
  n64ddipl.o2r                 built on device from the IPL
  gdiffuser.o2r                the engine archive, from the APK
  decomp-recipes/              Torch's extraction recipes, from the APK
  shaders/  fonts/             post-processing presets and the GUI fonts, from the APK
  mods/                        drop .o2r packs here
  saves/                       fzerox.sav, the 64DD journal, achievements
  ghosts/                      exported .gdg replays
  gdiffuser.cfg.json           settings, including every enhancement toggle
```

Uninstalling the app deletes all of it. Back up your saves first — the **Saves** screen, reached
from the in-game menu, writes one archive covering both folders.

> The 64DD journal in `saves/` is the important one. On an Expansion Kit build it, not the
> cartridge save, is where course edits, custom machines and most records live.

## Controls

The on-screen pad drives a **virtual SDL game controller**, so it goes through libultraship's
normal input mapping and can be remapped in the engine's own input editor exactly like a
physical pad. A Bluetooth or USB controller works with no setup at all.

The layout is built for a racer rather than for the N64 pad's own geometry: accelerate and brake
take the thumb corner, boost and the side attacks take the shoulders, and the camera and d-pad
are small and out of the way. **Menu** opens the enhancement menu, and the small button left of
centre hides the pad for a clean screen.

Course Edit is driven with an absolute mouse cursor, so a USB or Bluetooth mouse is what makes
the track editor usable here.

## Building

```sh
git clone --recursive https://github.com/Zorkats/G-Diffuser.git
cd G-Diffuser
android/gradlew -p android assembleDebug
```

The APK lands in `android/app/build/outputs/apk/debug/`.

| Prerequisite | Notes |
| --- | --- |
| **JDK 17–25** | Which includes the one current Android Studio bundles, so `./gradlew` works with no `JAVA_HOME` of your own. |
| **Android SDK 36**, **NDK 30.0.15729638** | Both pinned in `android/app/build.gradle.kts`. |
| **Gradle 9.7.1**, **AGP 9.3.2** | Fetched by the wrapper; nothing to install. They move as a pair — `android/gradle.properties` says why. AGP 9 supplies Kotlin itself, so there is no separate Kotlin plugin to apply. |
| **CMake 3.30.3** | Installed through the SDK manager. The NDK bundles 3.22, which is older than the 3.26 libultraship requires. |
| **Python 3** with PyYAML and Pillow | Generates the engine archive and the Expansion Kit compile assets during the build. |

Release signing reads `android/key.properties` if it exists, and the environment
(`KEYSTORE_FILE`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`) otherwise. With neither, a
release build falls back to the debug key so a fresh clone still produces an installable APK,
and the build prints a warning saying so: never publish that APK. See
`android/key.properties.example`.

### Making a signed release in Android Studio

1. Open the `android/` folder (not the repository root) in Android Studio and let it sync.
2. **Build → Generate Signed App Bundle or APK… → APK**.
3. On the key screen, **Create new…** makes the keystore. Keep the `.jks` outside the
   repository (it is gitignored either way) and back it up together with both passwords. Every
   future update has to be signed with this same key, or it will not install over the old one.
4. Pick the **release** variant and finish. The APK lands in `android/app/release/`.

To make later command-line builds (`./gradlew assembleRelease`) use the same key, copy
`android/key.properties.example` to `android/key.properties` and fill in the keystore path,
alias and passwords.

Before each release, raise `versionCode` (by at least 1) and set `versionName` in
`android/app/build.gradle.kts`. Android refuses an update whose `versionCode` is not higher
than the installed one.

`.github/workflows/android.yml` builds the same debug APK on CI and checks that it really
carries its game data before publishing the artifact.

---

## How it differs under the hood

Five things about this platform needed real work rather than a recompile. They are documented
here because each one is a place where a future change could quietly break the Android build
without breaking anything else.

### The extractor is linked in, not spawned

On desktop, `gdx-extract` is a separate deterministic executable and first boot spawns it as a
child process. An APK has nowhere to put a second executable: native code is loaded from a
directory nothing may write to, and W^X has blocked `exec()` out of app-writable storage since
API 29.

So Torch is compiled into `libmain.so` (`USE_STANDALONE=OFF`, which also makes it resolve spdlog
through the copy libultraship already fetched) and entered through
`port/android/gdx_extract_entry.cpp` — which is Torch's own `main()`, renamed, not a
reimplementation of it, so the argv the launcher builds means the same thing on both platforms.

The launcher still **forks** for it. That is deliberate: the fork keeps the exit code, the
stdout pipe the progress parser reads, and — the reason it matters on a phone — the peak.
Extraction holds the ROM and every decoded asset at once, and a child that `_exit()`s hands all
of it straight back. What Android drops is only the `exec()`.

### There are no ucontext fibers

The cooperative scheduler runs the game's real N64 threads as fibers on one OS thread. Bionic
ships `<ucontext.h>` as an alias for the mcontext *types* and implements none of
`getcontext`/`makecontext`/`swapcontext`, so the POSIX backend does not link here at all.

`port/gdx_fiber_asm.c` is that backend with the one missing primitive supplied by
`port/gdx_fiber_switch.S` — save the callee-saved registers, park the stack pointer, adopt
another's, restore. A fresh fiber starts from a frame built by hand at the top of its own stack,
so its first switch is an ordinary restore-and-return.

> The frame arithmetic in the C file and the register list in the assembly have to agree
> exactly. If you touch one, read both.

### `/proc/self/exe` is a lie here

Six places in the port resolved their data directory from it. On Android it points at the
zygote (`/system/bin/app_process64`), because the game is a shared library the runtime loaded
rather than a program the kernel started. `port/android/gdx_android_paths.h` answers that
question once, for all of them, with the app's external files directory.

The one caller it does not serve is the address resolver in `port/n64_gfx_bridge.cpp`, which
needs the *mapping* of `libmain.so` rather than a directory, and gets it from `dladdr` instead.

### The file picker goes through the system

Scoped storage leaves libultraship's own ImGui file browser able to see little beyond the app's
own directory, so a dump in Downloads or on Drive would be unreachable. `PickRomFile` and its
siblings open the system document picker instead, block the render thread until the answer
arrives on the UI thread, and hand back a path to a copy in the cache.

### There is no executable to stage files next to

Every desktop build stages the engine archive, the recipes, the shaders and the fonts beside the
executable through post-build steps in `port/CMakeLists.txt`. Android has no such directory, so
Gradle assembles the same set into one zip inside the APK (`android/app/build.gradle.kts`) and
`GameAssets.kt` unpacks it into the app's files directory on first run. The zip is written
sorted and at a fixed timestamp, so its digest changes only when its contents do — which is what
stops an app update re-unpacking a byte-identical payload.

## Known limits

- **arm64 only.** Not a size decision: the port maps the N64's 32-bit address space onto host
  pointers and recovers host addresses from bit patterns, treating the 64-bit pointer as the
  wide side of that mapping throughout. Whether a 32-bit ABI is sound here is a question nothing
  has answered.
- **No Discord Rich Presence.** It compiles, and there is no Discord socket on a phone to reach.
- **Clamp-to-border post-processing passes** fall back to clamp-to-edge on devices without
  `EXT/OES_texture_border_clamp` — GLES did not get it into core until 3.2.
- **The frame interpolation** the desktop port uses to render above 60 Hz is available, but a
  phone GPU is a much tighter budget than a desktop one; expect to leave it off on mid-range
  hardware.
