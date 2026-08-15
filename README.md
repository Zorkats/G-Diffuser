<div align="center">

<img src="assets/branding/gdiffuser-logo.png" alt="G-Diffuser" width="640">

**A native PC port of F-Zero X (N64), including 64DD Expansion Kit support.**

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)](#platforms)
[![Renderer](https://img.shields.io/badge/renderer-D3D11%20%7C%20OpenGL-8A2BE2)](#platforms)
[![Built on](https://img.shields.io/badge/built%20on-libultraship-informational)](https://github.com/Kenix3/libultraship)
[![Decomp](https://img.shields.io/badge/decomp-inspectredc%2Ffzerox-brightgreen)](https://github.com/inspectredc/fzerox)
![Discord](https://img.shields.io/badge/Discord-https%3A%2F%2Fdiscord.gg%2FgpaM8qK9NK-informational?style=flat)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

---

G-Diffuser runs F-Zero X natively on your PC — no emulator at runtime. It is a fully decompiled source port
built on top of the [inspectredc/fzerox](https://github.com/inspectredc/fzerox)
matching decompilation and the [Kenix3/libultraship](https://github.com/Kenix3/libultraship)
runtime (the same engine behind Ship of Harkinian and Starship, along with other PC ports like
Battleship). On top of the original
game it adds true widescreen rendering, an in-game enhancement menu, quality-of-life features,
texture-pack modding, and full support for the **64DD Expansion Kit** — Course Edit and the DD
cups included.

The repository ships **no** Nintendo ROM, disk, IPL, or extracted game assets. You bring your own
legally obtained dumps; game data is extracted or loaded locally from the files you supply.


## Features

- **True widescreen** — the game renders in 16:9 (`gEnhancements.Graphics.Widescreen`), with an
  optional **widescreen-anchored HUD** so on-screen elements sit at the screen edges instead of
  being stretched from 4:3 (`gEnhancements.Graphics.WidescreenUI`).
- **Ultrawide support (21:9 and wider)** — an opt-in mode widens the game's own culling so track
  pieces, machines, fireworks and background stars stop popping at the edges of ultrawide
  windows; the HUD anchors to the true corners (`gEnhancements.Graphics.UltrawideMode`).
- **Black border removal** — the original game framed every screen inside the CRT overscan-safe
  area, which shows as a black border on modern displays. An opt-in toggle opens the picture to
  the true edges on every screen (`gEnhancements.Graphics.RemoveBorders`), with a companion
  toggle to hide the race-intro wipe curtain (`gEnhancements.Graphics.HideRaceCurtain`).
- **High-refresh frame interpolation** — the simulation runs at its true 60 Hz while rendering
  interpolates smoothly up to your monitor's refresh rate (144 Hz and beyond), covering the
  camera, racers, effects and UI. WARNING: NOT PERFECT: While I did my best efforts to implement this in full capacity, it might get some slowdowns rarely, especially after screen transitions. I will do my best to keep optimizing this feature in future releases.
- **Discord Rich Presence** — off by default; when enabled it shows your mode, course, lap and
  position on your Discord profile, with per-field privacy toggles so you choose exactly what is
  visible (`gEnhancements.Online.DiscordPresence`).
- **In-game enhancement menu** — a full ImGui menu opened with **F1** (also Escape or Gamepad
  Back). Graphics, audio, gameplay, practice, ghosts and workshop tabs, keyboard- and
  controller-navigable.
- **64DD Expansion Kit** — loads the translated EK disk image and the 64DD IPL ROM to unlock the
  **Course Edit** track editor and the DD cups.
- **Ghost library** — import/export ghost replays as `.gdg` files, a per-course host-side ghost
  library, and a **Ghost Browser** window, all alongside the game's own SRAM ghost slot.
- **Photo mode** — hide the HUD while paused in a race for clean captures
  (`gEnhancements.Practice.PhotoMode`).
- **Practice lap deltas** — your last lap versus your session best, drawn in Practice mode
  (`gEnhancements.Practice.ShowLapDeltas`).
- **Texture-pack modding** — drop `.o2r` packs in a `mods/` folder to override textures
  (`gEnhancements.Workshop.TexturePacks`), with hot reload from the menu.
- **Asset dumping** — decode named assets straight from the extracted archive, per class, from the
  Workshop tab. See `docs/MODDING_GUIDE.md` for which dumper to use when building a pack.
- **Durable 64DD save sidecar** — Course Edit saves and disk writes are journaled to a sidecar
  file next to the game; your original `.ndd` disk image is **never** modified.
- **Modern rendering knobs** — internal resolution scale, MSAA, texture filtering, VSync,
  z-fighting mitigation, draw distance, and frame pacing.
- **Input** — keyboard and SDL controller support (including DualSense), plus a draggable
  on-screen **N64 input viewer** overlay.
- **Developer tools** — live Stats, an optional top-right FPS/frame-time overlay, a command
  Console, and the Fast3D graphics debugger under **Dev Tools**.

## Planned features

All of the vanilla content — cartridge and Expansion Kit — is playable from start to finish
today. These are the next things planned for G-Diffuser:

- **Mouse control in Course Edit** — point-and-click track editing instead of driving the cursor
  with a pad.
- **Content export and import** — share a custom track or a Create Machine vehicle as a single
  file another player can drop in and race, author ghosts included.
- **Better modding support** — per-tile font/atlas overrides and paletted (CI4/CI8) texture
  encoding, the two big gaps in what packs can replace today.
- **Players 2–4 on gamepads** — full multi-controller support for VS and split-screen.
- **Full controller navigation** of the enhancement menu.
- **Japanese version support** — the original JP ROM and disk as a selectable profile.

## Quick Start

> **You must provide your own legally-obtained F-Zero X ROM. G-Diffuser does not, and will
> never, distribute copyrighted game files.**

1. **Get the game files you own.** G-Diffuser needs an F-Zero X **US rev0** ROM dump. It must be
   **big-endian (`.z64`) byte order** — the loader does not byte-swap, so a `.n64` or `.v64` dump
   must be converted to `.z64` first or it will be rejected. Setup auto-detects a ROM named
   `baserom.us.rev0.z64` beside the executable; any other filename works too, as long as you point
   the first-boot wizard at it with **Browse** — it is copied in under the canonical name.
2. **Download** a G-Diffuser release for your platform, or build it yourself (see
   [Building](#building)).
3. **Launch** the executable. On first boot the setup screen detects files already beside the
   executable and lets you review, replace, or choose the ROM, Expansion Kit disk image, and 64DD
   IPL ROM. Extraction begins only after you select **Build game data and continue**. Everything
   remains beside the executable so later launches boot directly into the game.
4. **Play.** Press **F1** at any time to open the enhancement menu.

### Expansion Kit files (required)

G-Diffuser is a port of the full Expansion Kit experience — Course Edit, the DD cups, and the
disk save system are core features, not add-ons — so these two files are required alongside the
ROM:

| File | What it is |
| --- | --- |
| `baserom.translated.ek.ndd` | The fan-translated Expansion Kit disk image (English translation by Zoinkity, adapted to the 64DD disk image by LuigiBlood). The loader also accepts `baserom.jp.ek.ndd` for the original Japanese disk. A retail 64DD image is ~64.9 MB. |
| `N64DDIPLROM.n64` | A 64DD IPL / drive ROM dump (~4 MB). Supplies the drive's built-in font. The US prototype dump is also accepted under its own name, `64DD_IPL_US_MJR.n64` — no renaming needed. |

Place them next to your ROM (or feed them to the first-boot wizard). Setup will not complete
without them.

## Controls

Open or close the enhancement menu with **F1**, **Escape**, or **Gamepad Back**. Toggle
fullscreen with **F11**. Menu navigation from a controller is opt-in via the menu's
controller-navigation toggle.

## Platforms

| Platform | Renderer |
| --- | --- |
| **Windows** | Direct3D 11 |
| **Linux** | OpenGL (SDL2) |

Both targets are driven by libultraship's Fast3D renderer.

**Linux requirements:** glibc 2.35 or newer — Ubuntu 22.04+, Debian 12+, Fedora 36+, SteamOS 3,
or any rolling distribution. No CPU features beyond the x86-64 baseline are needed, so anything
64-bit will do. Libraries that distributions do not reliably ship travel in `lib/` beside the
executable, so nothing has to be installed first.

## Custom Assets

The release includes `gdiffuser.o2r`, which contains only the MIT-licensed Fast3D shaders needed
to initialize the renderer. On first boot, `gdx-extract` uses your ROM and the CC0-licensed
`decomp-recipes/` metadata to generate your local `fzerox.o2r`. That generated archive contains
game-derived data and must not be redistributed with G-Diffuser.

To mod textures, place additional `.o2r` packs in a `mods/` folder next to the game; enable
**texture packs** in the Workshop tab and reload. You can build packs from the game's own textures
using the built-in **texture dump** feature. A numeric filename prefix (for example,
`10-hifonts.o2r`) controls load order.

## Building

G-Diffuser is a CMake project. It pulls in four components: `libultraship/`
(Kenix3/libultraship — runtime + Fast3D renderer), `torch/` (HarbourMasters/Torch — build-time
asset extraction), `decomp/` (inspectredc/fzerox — the F-Zero X C source), and
`fzerox-expansion-kit/` (the Expansion Kit decompilation reference). Clone with
submodules:

```sh
git clone --recursive https://github.com/Zorkats/G-Diffuser.git
```

### Windows

| Prerequisite | Notes |
| --- | --- |
| **Visual Studio 2022** | MSVC toolset, "Desktop development with C++" workload |
| **CMake 3.24+** | The top level asks for 3.20, but `libultraship/` requires 3.24 |
| **Python 3** | Required, not optional — CMake configure fails without an interpreter (build-time asset generators). The generators also need **PyYAML** and **Pillow** |

> Build from a clean shell **without** MSYS2/MinGW on your `PATH`, or MSVC may pick up MinGW
> headers. Use a Developer Command Prompt (`vcvars64`).

```sh
# Configure. Visual Studio is CMake's default generator on Windows; USE_AUTO_VCPKG lets
# libultraship bootstrap its own vcpkg into the build tree, so no manual vcpkg install is needed.
cmake -S . -B build/x64 -DUSE_AUTO_VCPKG=ON

# Build. --config Release is REQUIRED here: the Visual Studio generator is multi-config, so it
# ignores -DCMAKE_BUILD_TYPE and picks the configuration at build time.
cmake --build build/x64 --config Release --target G-Diffuser
```

The executable lands in `build/x64/port/Release/`. Other targets: `ALL_BUILD` builds everything
including the console test executables; `G-Diffuser-JP` exists only when you configure with
`-DGDX_BUILD_JP=ON`.

<details>
<summary><b>Alternative: Ninja</b></summary>

Ninja is single-config, so `-DCMAKE_BUILD_TYPE` selects the configuration and `--config` does
nothing. libultraship's auto-vcpkg derives its triplet from the Visual Studio generator, so on the
Ninja path supply your own vcpkg instead:

```sh
vcpkg install zlib bzip2 sdl2 glew libzip nlohmann-json tinyxml2 spdlog --triplet x64-windows

cmake -S . -B build/x64-ninja -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build/x64-ninja --target G-Diffuser
```

</details>

### Linux

Install the toolchain and libultraship's dependencies from your distribution (CMake 3.24+, Ninja, a
C++20 compiler, Python 3, SDL2, GLEW, zlib, bzip2, libzip, nlohmann-json, tinyxml2, spdlog), then:

```sh
cmake -S . -B build/x64-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/x64-linux --target G-Diffuser
```

### Expansion Kit build

Expansion Kit support is **on by default** (`GDX_EXPANSION_KIT=ON`) and is the supported
configuration — it is what makes the disk loader, Course Edit, and the DD cups part of the build,
and it is why the [Expansion Kit files](#expansion-kit-files-required) are required at runtime.

Configuring `-DGDX_EXPANSION_KIT=OFF` produces a cartridge-only binary with the disk subsystem,
Course Edit, and the DD cups excluded. That build does not need the disk image or IPL, but it is not
what releases ship and it is not the experience this port is for.

## FAQ

**Do I need an emulator?** No. G-Diffuser is native code; there is no emulator at runtime.

**Where do I get the ROM / disk / IPL?** You must dump them yourself from hardware you own. This
project does not condone piracy and will not point you to copyrighted files.

**Which ROM region?** F-Zero X **US rev0**. Use a `.z64` (big-endian) dump. The loader does not
byte-swap — `.n64` / `.v64` images must be converted to big-endian `.z64` first, or they'll be
rejected at load time.

**Will my disk file get corrupted?** No. Disk writes go to a separate save sidecar; your original
`.ndd` image is never modified.

**Is the `docs/` folder part of the release?** Yes. `docs/` holds the maintained user-facing
documentation that ships with releases — the architecture overview, the diagnostics reference,
the modding guide, and the status ledger. Internal design notes and investigation material live
outside the repository.

## Troubleshooting

**The game runs too fast.** On a high-refresh display (e.g. a 120/144 Hz laptop or handheld) where
vsync isn't capping the frame rate, the game can run faster than 60 FPS. Open the enhancement menu
(**F1**), go to the graphics settings, and enable the **Frame Pacer** — it holds the game to its
intended 60 FPS. The setting persists across launches.

**Diagnostic logging.** Log output is opt-in; without it, no run log is created. Two ways to turn it
on:

| How | What it does |
| --- | --- |
| **Environment variables** | `GDX_LOG=1` writes `gdiffuser-run.log` next to the executable. `GDX_TRACE=1` adds the high-frequency per-frame trace lines (and opens the log file on its own). A variable exported at launch pins that setting for the whole run, including boot, and the menu shows it as locked. |
| **In-game** | **F1 → Dev Tools → General → Developer gates → Logging**: *Write gdiffuser-run.log* and *High-frequency trace*. These persist in `gdiffuser.cfg.json`, but they are read at startup — tick one and **restart** to capture boot. |

> **`GDX_TRACE` is off by default in release builds** (it is on by default in Debug builds). That is
> usually why a log has none of the interesting per-frame lines in it. Set it explicitly.

Those two are what a bug report needs. The full set of 28 developer gates is reference material: it
lives on that same Dev Tools page, and in `port/gdx_dev_gates.c` in the source, each with its own
description.

**Crash reports.** On Windows a crash always writes `gdiffuser-crash.txt` next to the executable —
no environment variable required. Attach it if you have one.

## Reporting a bug

Open an issue at [github.com/Zorkats/G-Diffuser/issues](https://github.com/Zorkats/G-Diffuser/issues).

Generate a log first:

```sh
# Windows (PowerShell)
$env:GDX_LOG="1"; $env:GDX_TRACE="1"; .\G-Diffuser.exe

# Linux
GDX_LOG=1 GDX_TRACE=1 ./G-Diffuser
```

Then reproduce the problem, quit, and include:

- [ ] **Platform and renderer** — Windows/D3D11 or Linux/OpenGL
- [ ] **Which build** — release version, or the commit you built
- [ ] **ROM region and dump** — F-Zero X US rev0, plus whether the Expansion Kit disk is the
      translated or the Japanese image
- [ ] **Repro steps** — what you did, what you expected, what happened
- [ ] **`gdiffuser-run.log`** — from the run above, found next to the executable
- [ ] **`gdiffuser-crash.txt`** — if the game crashed

Please do not attach ROMs, disk images, IPL dumps, or your generated `fzerox.o2r`.



## Regarding LLM Usage.

To be fair, I was scared at first of saying I used LLM models to bring G-Diffuser into existence. I used LLM models to better understand how F-Zero X's quirks worked (it's probably one of the most complex N64 games) and yes, I used it to write code, but 70% of the code was written by me,  and 30% was written by an LLM but that code was later reviewed and tested by me, with every single iteration being a testing process. I also audited and made every single design and choice regarding the software, the models weren't involved in any of the ideas and thought processes I had during development. This was also tested by me and my older brother, each doing their own playthroughs of the game with this port.  I know some people (including people I have worked with before, and myself) will not be content with me using LLMs to accelerate a development process that would have taken more months, but the ones that know me, and know the work I have done for different communities is more worth it than me just using an LLM model to understand the game's source code and improve the workflow. I am a computer science student, modder for various games, and have made various hand made contributions to other projects, and this was a vacation project for me in order to distract myself from my own real world troubles. And this would not have been possible without the decompilation of inspectrdc, I am grateful to them and their team.  I hope that the quality of the project makes people play it and have a good experience. But if you don't want to play a project that was done with the help of an LLM, that's 100% okay, but just walk away and don't throw hate at me. I have enough with my own daily woes. A project that will be done 100% by hand will appear once the decomp reaches 100%, but in the meanwhile you can have this. I do believe there's a difference between making "slop" and using LLMs to automate and help with certain tasks. And that is probably what is going to matter from now on, having critical thinking to know when the model has made mistakes and know how to fix them yourself (or redirect the model in doing it). And to be clear, I AM against LLM generated art or assets. But code is different. There's a lot of difference between writing 200+ lines of DirectX boilerplate or header files and make the model write them, for example, while I focus on other tasks, than making the LLM do EVERYTHING. People will not realize that this wasn't a "generate me an F-Zero X port" prompt, there was a PRD (Product Requirement Document) made by hand by me, with the whole structure, and analysis, and code done beforehand before I even started using LLMs to code. 

## Credits

G-Diffuser stands entirely on the shoulders of the people who did the hard foundational work.

- **[inspectredc](https://github.com/inspectredc) and the [fzerox decompilation](https://github.com/inspectredc/fzerox) contributors** — without their matching decompilation of F-Zero X, none of this would exist. This port is built directly on their source, and I am deeply grateful to them and their team. F-Zero X is one of the most complex N64 games to reverse-engineer, and their work is the whole reason G-Diffuser can run at all.
- **Zoinkity** — for the original F-Zero X Expansion Kit English translation.
- **[LuigiBlood](https://github.com/LuigiBlood)** — for adapting and improving that translation into the 64DD disk image G-Diffuser loads, and for the extensive 64DD research and preservation work that makes the Course Edit / DD-cup support possible.
- **[Kenix3](https://github.com/Kenix3) and the [libultraship](https://github.com/Kenix3/libultraship) project** — the runtime and Fast3D renderer G-Diffuser is built on.
- **The [HarbourMasters](https://github.com/HarbourMasters) community** — [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) and [Starship](https://github.com/HarbourMasters/Starship) were both the inspiration for this project and the source of much shared technology and know-how.
- **[Kiziio](https://github.com/Kiziio1)** — for the G-Diffuser logo and icon artwork.
- **The wider N64 decompilation and modding community** — for the tools, documentation, and years of accumulated knowledge that make projects like this achievable.

## License and legal

Original G-Diffuser code and documentation are available under the [MIT License](LICENSE).
Submodules, vendored components, tools, and fonts retain their own licenses; binary distributions
include their complete texts under `LICENSES/`. See [Third-party notices](THIRD_PARTY_NOTICES.md)
for the component and asset boundary.

This project is an unofficial compatibility project and is not affiliated with, endorsed by,
sponsored by, or associated with Nintendo. F-Zero and Nintendo are trademarks of Nintendo.
G-Diffuser distributions contain no Nintendo ROM, disk, IPL, texture, model, audio, or other game
payload. Users must supply legally obtained dumps. The locally generated `fzerox.o2r` and any
texture dumps remain game-derived and must not be distributed with the project.
