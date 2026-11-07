# G-Diffuser Companion

The Companion is a separate desktop program that builds the files G-Diffuser loads: replacement
music, texture packs, machine model packs, native audio packs, and track files from other players'
disk images. It also opens packs other people made, so you can study them. It never edits the
game's settings, saves or stores. It writes only the files you ask it to build, and it tells you
which game option turns each one on.

> **Platforms.** The Windows x64, Linux x64 and Linux arm64 downloads include the Companion in a
> `companion` folder. It is tested on Windows x64 and on Linux x64 (KDE Plasma on Wayland). Linux
> arm64 builds pass the same automated checks but have not been tried on a device yet. macOS
> downloads do not include it: it is unverified there.
>
> On Linux, **Browse...** opens `kdialog` on KDE and `zenity` elsewhere. Install one of them
> (most desktops already have one), or type the paths into the fields. To pick one yourself, set
> `GDX_COMPANION_DIALOG` to `zenity`, `kdialog` or `none` before starting the Companion.

## Quick path

1. Put the `companion` folder inside your G-Diffuser folder, so `G-Diffuser.exe` sits one folder
   above `G-Diffuser-Companion.exe` (`G-Diffuser` and `G-Diffuser-Companion` on Linux). Release
   downloads already have this layout.
2. Start `G-Diffuser-Companion.exe`. The status bar should say **Installation: Validated** and
   **Worker: Ready**. If the game is somewhere else, open **Settings > Installation** and choose
   `G-Diffuser.exe`.
3. Pick a workspace on the left (Music, Textures, Models, Tracks, Inspect or Advanced), fill in the form,
   and press **Build** (or **Install** for streamed music). Each problem is shown under its field,
   and Build stays off until every problem is fixed.
4. Turn the result on in the game, as the page tells you, then restart the game if the page says
   so.

## Workspaces

| Workspace | What it makes | How the game uses it |
| --- | --- | --- |
| Music | A streamed recording in `music/`, or a native sample pack | Restart the game. Native packs need **Workshop > Mods > Enable sample packs**. |
| Textures | A texture pack `.o2r` from a folder of replacement images, or from a `.htc` texture cache | **Workshop > Mods > Enable texture packs**, then tick the pack |
| Models | A machine model pack `.o2r` that replaces a machine's static body | **Workshop > Mods > Enable model packs** |
| Tracks | A `.gdxc` track file read from a 64DD disk image | **Workshop > Content Library** imports it from `exports/` |
| Inspect | Nothing in the game: it lists what someone else's `.o2r` or `.gdxp` holds, and unpacks it into a folder you can study | Not loaded by the game |
| Advanced | Sequence, soundfont and native sample packs in the game's own audio formats | The matching **Enable ... packs** option in **Workshop > Mods** |

The **Workshop** names above are pages of the game's own menu, which opens with **F1**.

### Music

- **Target.** Replace a game song (pick its slot), or add a course song under a name of your own.
  Course songs are assigned to a track in Course Edit; the Companion does not assign them. A few
  slots play through the sound effect player and ignore `music/`; the list marks them as not
  replaceable.
- **Recording.** Ogg Vorbis, FLAC or WAV, read with the game's own decoders, so a file that opens
  here plays in the game. MIDI files are not converted.
- **Loop.** Keep the file's loop tags, repeat the whole recording, or choose the loop. All three
  work for Ogg Vorbis, FLAC and WAV. The Companion writes `LOOPSTART` and `LOOPLENGTH` into an
  Ogg or FLAC and a `smpl` loop into a WAV, without re-encoding the audio.
- **Loop editor.** The waveform shows the loop as a shaded region between the **Start** and
  **End** markers.

  | Action | Result |
  | --- | --- |
  | Drag a marker | Moves it. On release it snaps to the nearest zero crossing within 10 ms, unless **Snap dragged markers to zero crossings** is off. |
  | Click the waveform | Plays from that point. |
  | Mouse wheel | Zooms around the pointer. **Show all**, **Show the start** and **Show the end** jump the view. |
  | Shift + wheel, or right drag | Scrolls. |
  | Type a time | Each marker takes `m:ss.mmm` (or plain seconds) or an exact frame. **Nudge step** sets how far the frame arrows move it. |

  **Seam match** scores how well the end joins the start, and the seam view draws what you will
  hear across the jump next to what the recording would have played instead. **Match the end to
  the start** moves the end up to 50 ms so the two line up.
- **Find loop points.** Compares the recording with itself and lists up to five loops, best
  first, with their seam match. **Hear** plays a suggestion's seam and **Use** takes it into the
  editor. It needs a recording of at least 12 seconds and suggests loops of 5 seconds or more.
  A suggestion with a low seam match is a starting point to adjust, not a finished loop.
- **Level.** With **Match the game's music level** on, as it is by default, the install measures
  the recording's loudness (EBU R128) and writes it into the file as the level tags the game
  reads, so the song plays as loud as the game's own music. The audio is not re-encoded or
  changed. **Adjust** plays the song up to 12 dB louder or quieter than that. Once the waveform is
  read, the page shows the measured loudness and how far the game turns the song down or up. The
  game never raises a song so far that its peak passes full scale. With matching off, the file's
  own level tags, if any, stay as they are. A silent recording cannot be matched.
- **Preview.** **Play** plays the recording with the chosen loop, at the file's own level. **Hear the loop seam** starts
  three seconds before the loop end, so you hear the jump back.
- **Install.** The page lists the destination, the files it will replace, and the older backups it
  will delete. Files it replaces are renamed to `<name>.previous`, which the game ignores, and
  **Restore the earlier version** swaps them back. The new file is written to a staging folder,
  read back to the end with the game's decoders, and only then moved into place. If any step
  fails, every moved file goes back.
- **Advanced: native samples.** Only the title, selection and Expansion Kit Big Blue songs can be
  replaced this way. The page shows the resources the pack replaces, their sample rates, and the
  native memory the recording needs. All native sample packs share 1 MiB, and Build stays off
  when the recording does not fit. It needs the game's audio dump (**Workshop > Tools > Asset
  Dump**).

See [Custom music](modding/CUSTOM_MUSIC.md) for the slot names and the loop and level tags the game
reads.

### Textures

- **Texture pack.** Start from the game's texture dump (**Workshop > Tools > Asset Dump**), edit
  the dumped images, keep their folders and the dump's `manifest.tsv`, and pack the folder.
  **Validate** lists the textures you changed and every file that matches no game texture, so you
  never need to type resource keys into file names.
- **Convert a .htc texture pack.** Converts a `.htc` texture cache into a texture pack.

See [Texture packs](modding/TEXTURE_PACKS.md).

### Models

A model pack replaces the static body of a machine. Wheels, boosters and effects stay as the game
draws them. **Create a new project** starts a project folder for one machine from the game's dump.
Machines the dump did not capture cannot be rebuilt. See [Machine models](modding/MACHINE_MODELS.md).

### Tracks

Choose a 64DD disk image (`.ndd`, `.disk` or `.ram`) and press **Read the disk**. The table lists
each Course Edit track with the game's own checks:

| Result | Meaning |
| --- | --- |
| Fits | The track passes the game's checks. |
| Needs Extended course height | It passes only with **Enhancements > Course Edit & Machine > Extended course height (experimental)** on. |
| Refused | The game would reject it; the details say why. |

**Export** writes the selected track as a `.gdxc` file, by default into the game's `exports/`
folder, where **Workshop > Content Library** lists it for import. A file saved elsewhere must be
copied into `exports/` first. Reading never changes the disk image. Machines on the image are
listed but not exported; the game's own disk image import covers those. See
[Sharing tracks and machines](modding/SHARING_TRACKS_AND_MACHINES.md).

### Inspect

Use Inspect to see how someone else built a pack. It reads `.o2r` packs (textures, models, audio)
and `.gdxp` track packages, and never changes the file it reads.

1. Choose the pack. Optionally choose the game's texture dump folder (**Workshop > Tools > Asset
   Dump**). The page fills in the game's `dump` folder when it finds one.
2. Press **Read the pack**. The page shows the pack's name, author, version and description, a
   summary by kind, and every entry with its kind and detail (a texture's format and size, a file's
   byte count). **Filter** narrows the list.
3. Press **Unpack** to copy it into a new folder. The page proposes `<pack name>-unpacked` beside
   the pack; **Browse...** picks a different parent folder. The folder must not exist yet.
   **Open folder** opens the result.

The unpacked folder holds:

| Path | Contents |
| --- | --- |
| `contents.tsv` | One row per entry in the pack: its path, kind, detail, and where it was written. |
| `workshop.json` | The pack's metadata, as stored. |
| `textures/` | Texture overrides as PNG images, laid out like the game's dump, with a `manifest.tsv`. The Textures page packs this folder again. |
| `alt-textures/` | The same for textures on the alternate assets channel. |
| `palette-variants/` | Palette variants as PNG. |
| `files/` | Every other entry (Lua scripts, JSON, audio, models) byte for byte, at its path in the pack. |

Paletted textures (CI4 and CI8) store colour indices, and the colours come from the game. With the
dump folder chosen, they unpack in their real colours and keep the dump's native size and format.
Without it they still unpack, with their indices shown as grey levels.

A pack is someone else's file, so it is treated as untrusted. An entry with an unsafe name is
listed and skipped, sizes and counts are capped, and nothing is written outside the new folder. A
cancelled unpack leaves no folder behind.

The same reader runs from a terminal, with Python 3 from the G-Diffuser source tree:

```powershell
python -I tools\unpack_pack.py .\their-pack.o2r --list
python -I tools\unpack_pack.py .\their-pack.o2r -o .\their-pack-unpacked --dump-dir "C:\Games\G-Diffuser\dump"
```

`--json` prints the result as JSON. Reading the dump's palettes needs Pillow; without it, paletted
textures show their indices in grey.

### Advanced

Sequence, soundfont and native sample packers, in the game's own audio formats. A sequence is note
data in the N64 format. To replace a song with a recording, use Music instead. See
[Instruments](modding/INSTRUMENTS.md) and [Sound effects](modding/SOUND_EFFECTS.md).

## Projects

The Textures, Models and Advanced pages can save their form as a project file (`.gdxproj`), so the
same build can run again later. A project is written only when you press **Save** or **Save
as...**. Paths are stored relative to the project file's folder, so a project folder can move as a
whole. When a file a project names is gone, the page lists it as **Missing** so you can choose it
again.

## Long jobs

Building runs in a separate worker program, `worker/gdx_companion_worker.exe`. While a job runs the
page shows its progress and a **Cancel** button. Cancel stops the worker and deletes the job's
staging folder. A pack is published only after it was checked, and the pack it replaces is kept as
`<pack>.previous`, so a cancelled build never loses the last good pack. After a cancel, press
**Start the worker again**.

## Themes

The Companion's look comes from themes. Put a folder with a `theme.json` and an optional backdrop
image inside `companion/themes/`, then press **Rescan** in **Settings**. **Open folder** shows the
themes folder. Start with `--theme user.<folder>` to open with a theme.

## Command line

```text
G-Diffuser-Companion [--section <music|textures|models|tracks|inspect|advanced|settings>] [--theme <id>]
G-Diffuser-Companion --selftest
G-Diffuser-Companion --python <interpreter> --worker <worker.py>
```

| Option | Use |
| --- | --- |
| `--section` | Opens on that workspace instead of Settings. |
| `--theme` | Starts with that theme id. Not saved. |
| `--selftest` | Starts the worker, checks its handshake and shutdown, prints the result, and exits 0 on success. |
| `--python`, `--worker` | Development only: runs `companion/worker/gdx_companion_worker.py` with that interpreter instead of the frozen worker. |

The only setting the Companion saves is the game folder chosen in Settings, in
`companion-settings.json` beside the Companion.

## Building it

1. Configure and build the repository as usual. The `G-Diffuser-Companion` target builds the
   program and copies `tools/music_targets.json` beside it.
2. Freeze the worker: `python -I tools/build_companion_worker.py`. This needs Python 3.11 or newer.
   It installs pinned PyInstaller and Pillow into a virtual environment under `build/`, freezes the
   worker into `build/companion-worker/`, and smoke-tests it with Python removed from `PATH`.
3. Build `G-Diffuser-Companion-worker` to copy the frozen worker beside the program.
4. Package:
   - Windows: `pwsh tools/package_windows.ps1 -BuildDir build/x64 -Version <version> -Companion`
   - Linux: `bash tools/package_linux.sh --companion build <version> dist linux-x64`

   Both refuse to package when a Companion file or license is missing, when the program needs a
   library the package does not carry, or when the staged copy fails `--selftest`. On Linux the
   frozen worker must also pass the same glibc and CPU limits as the game
   (`tools/check_linux_abi.py --frozen-dir`), so freeze it on Ubuntu 22.04 for a release.

User themes in the build folder are test data and are never packaged.

## Third-party components

The Companion ships no game assets. Everything it builds comes from your own files and from the
game's dump on your machine.

| Component | License | Where it is | License text |
| --- | --- | --- | --- |
| Dear ImGui | MIT | statically linked into the program | `LICENSES/MIT-Dear-ImGui.txt` |
| SDL2 | zlib | statically linked into the program on Windows; on Linux the game's `lib/libSDL2-2.0.so.0` | `LICENSES/zlib-SDL2.txt` |
| stb_image | MIT or public domain | statically linked into the program | `LICENSES/MIT-stb.txt` |
| stb_vorbis | public domain, or MIT | statically linked into the program (the game's music decoders) | end of `port/third_party/stb_vorbis/stb_vorbis.c` |
| dr_flac, dr_wav | public domain, or MIT-0 | statically linked into the program (the game's music decoders) | end of each header in `port/third_party/dr_libs/` |
| Python 3.12 runtime (Windows: with OpenSSL, libffi, bzip2, xz, expat and mpdecimal) | PSF-2.0 and, on Windows, the components' own licenses | `worker/_internal/` | `worker/_internal/LICENSES/PSF-2.0-Python.txt` |
| Linux only: the Ubuntu 22.04 system libraries the worker carries (OpenSSL, zlib, bzip2, xz, libuuid) | each package's own licenses | `worker/_internal/` | `worker/_internal/LICENSES/debian-<package>.txt` |
| Pillow, with the image libraries its wheel bundles | MIT-CMU and the bundled libraries' licenses | `worker/_internal/PIL/` | `worker/_internal/LICENSES/MIT-CMU-Pillow.txt` |
| PyInstaller bootloader | GPL-2.0 with the bootloader exception, which allows shipping it with any program | `worker/gdx_companion_worker.exe` (`worker/gdx_companion_worker` on Linux) | none required |

PyInstaller and Pillow are build-time tools. Players do not need Python installed.
