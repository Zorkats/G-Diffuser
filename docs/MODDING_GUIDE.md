# G-Diffuser Modding Guide

> Change how F-Zero X looks, sounds, and plays, without touching the original game files.

Most mods are **packs**: single `.o2r` files you drop into the `mods/` folder. The game layers them
on top of the original assets while it runs, and deleting a pack restores the stock game. For
changes packs cannot make, you can edit the source code and build your own version.

## Choose a guide

| I want to... | Guide | Tools needed |
| --- | --- | --- |
| Repaint textures, logos, or the HUD | [Texture packs](modding/TEXTURE_PACKS.md) | Python, Pillow, an image editor |
| Replace a song with my own recording | [Custom music](modding/CUSTOM_MUSIC.md) | Python, an audio editor |
| Replace sound effects or the announcer | [Sound effects and voices](modding/SOUND_EFFECTS.md) | Python, an audio editor |
| Change the instruments of note-based music | [Instruments](modding/INSTRUMENTS.md) | Python |
| Replace a machine's 3D model | [Machine models](modding/MACHINE_MODELS.md) | Python, a 3D editor |
| Add screen filters such as CRT or scanlines | [Shaders](modding/SHADERS.md) | A text editor |
| React to races with scripts | [Lua scripts](modding/LUA_SCRIPTS.md) | A text editor |
| Share tracks and machines I built | [Sharing tracks and machines](modding/SHARING_TRACKS_AND_MACHINES.md) | None |
| Change gameplay, physics, or menus | [Code mods](modding/CODE_MODS.md) | A C/C++ build setup |
| Add a venue, song, or track effect to Course Edit | [Course elements](modding/COURSE_ELEMENTS.md) | A C/C++ build setup |

Every guide is self-contained. Start with the one that matches what you want to make.

## The basics

### Folders

These folders live next to the game executable. The game creates them when it needs them.

| Folder | What it holds |
| --- | --- |
| `mods/` | Installed `.o2r` packs. Scanned at boot and on **Reload packs**. |
| `dump/` | Assets written by **Workshop > Tools > Asset Dump**, for you to edit. |
| `shaders/` | Shader files and presets. See [Shaders](modding/SHADERS.md). |
| `exports/` | Shared `.gdxp` tracks (with their music) and `.gdxc` machines. See [Sharing](modding/SHARING_TRACKS_AND_MACHINES.md). |

### Pack tools

The pack builders are Python scripts in the `tools/` folder of the source code. They are not in
the release download. On the [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser),
click **Code**, then **Download ZIP**, and extract it anywhere. Players who install your pack do not
need Python or the tools.

Several tools read the game's dump. Copy the game's `dump/` folder into the extracted source folder
before you run them.

### Master switches

Each kind of pack has its own switch on **Workshop > Mods**. They are all **off by default**, so
installing a pack changes nothing until you turn its kind on.

| Switch | Turns on |
| --- | --- |
| **Enable texture packs** | Texture packs |
| **Enable sequence packs** | Sequence packs (note data for music) |
| **Enable sample packs** | Sample packs (recorded songs, sound effects, voices) |
| **Enable soundfont packs** | Soundfont packs (instrument sets) |
| **Enable Lua script packs** | Lua scripts |
| **Enable model packs** | Machine model packs |

After installing, enabling, or removing a pack, click **Reload packs**.

## Reference

### Pack metadata (`workshop.json`)

Every pack carries a small file named `workshop.json` at the top of the archive:

```json
{
  "name": "My Pack",
  "version": "0.2",
  "author": "you",
  "game_version": "us.rev0",
  "key_scheme_version": "2",
  "id": "you.my-pack",
  "depends": ["someone.base-textures"],
  "conflicts": ["someone.other-hud"]
}
```

| Field | What it does |
| --- | --- |
| `name` | Shown in the pack list. |
| `version` | Your pack's version. Free text. |
| `author` | Shown in the pack list. |
| `game_version` | The game build the pack targets. `us.rev0` is the current port. Any other value is flagged in red. |
| `key_scheme_version` | Texture packs only: the key scheme the pack was built for. A mismatch is flagged in red. |
| `id` | The pack's stable identity, used for load order and the disable list. No commas. Packs without one use their file name. |
| `depends` | Pack ids this pack needs. A missing one produces a warning only. |
| `conflicts` | Pack ids this pack clashes with. Loading both produces a warning only. |
| `files` | An integrity list of every file in the pack, with checksums. Written by the texture and model packers; never write it by hand. |
| `pack_type` | What the pack carries. Written by the packers. |

You rarely write this file yourself. Every packer writes it from its `--name`, `--author`,
`--version`, `--game-version`, and `--id` flags, and fills anything you leave out with safe
defaults. If you do provide a `workshop.json`, the flags you pass override its fields.

The file must be named `workshop.json`, **not** `manifest.json`. The engine reserves
`manifest.json` for its own archives, and a pack using that name fails to load.

### Load order

- The pack list on **Workshop > Mods** shows packs in load order. Its **Up** and **Dn** buttons
  change that order.
- When two packs replace the same thing, **the pack lower in the list wins**.
- Packs that are not in the saved order load after it, sorted by file name.
- Unticking a pack's checkbox stops it from loading on the next **Reload packs** or boot.

The order is saved as a list of pack ids in the `gEnhancements.Workshop.PackOrder` setting.

### Texture keys

Every texture has a key, and the key decides whether it can be replaced:

| Key looks like | Meaning | Replaceable? |
| --- | --- | --- |
| `common_assets_compressed/aTitleLogoTex` | A named texture | Yes |
| `atlas/machine_custom_gfx/aTimerSymbolsTex/o256/RGBA16/8x16` | One band of a multi-tile strip | Yes |
| `hash/97f5ac8c0cd4fb43` | An unnamed texture, identified by its pixels | No, dump only |

Named keys stay the same across builds. An `atlas/` key names one band of a strip the game loads
as a single tall image: `o<offset>` is the band's byte offset in that strip, followed by its format
and size. The dump writes one PNG per band, so you never type these keys yourself.

The current **key scheme version** is `2`. If a future build renames textures, the version goes up
and the pack list flags older packs. Packs built for scheme 1 do not match any atlas band in
scheme 2. They do no harm, but they need a fresh dump and rebuild to work again.

### Texture formats

The game decodes each texture in its original N64 format, which the dump records in the `fmt`
column of `manifest.tsv` (`RGBA16`, `I4`, `IA8`, and so on). You always edit ordinary RGBA PNG
files, and the packer converts them back to the original format.

Paletted textures (`CI4` and `CI8`) work at their original size only. The dump writes the
original palette as a small PNG named in the `palette_key` column, and the packer maps your colors
onto it. `--check` reports how far the colors moved. Paletted atlas bands cannot be replaced.

### Direct-key texture packs (advanced)

By default, a texture pack stores each image under `textures/pack/<key>`, and the game checks each
texture against those entries with safety guards. With `--direct-keys`, the packer instead stores
each image at the bare `<key>`. Because packs load after the game's own archives, that entry
replaces the original resource of the same name everywhere it is used.

```sh
python tools/gen_texture_pack.py <your-edit-folder> my-pack.o2r --name "My Pack" --direct-keys
```

- The default channel's guards do not apply. Test with `--check`, and prefer the default unless
  you need this.
- `hash/` keys are refused: they name no original resource.
- Atlas strips still need the default channel's `atlas/` band keys. Direct keys do not reach them.
- **Enable texture packs** still controls direct-key packs.

### Current limitations

1. Unnamed (`hash/`) textures cannot be replaced.
2. Paletted atlas bands cannot be replaced.
3. Model packs replace only the 30 stock machine bodies: not Create Machine machines or parts,
   crash debris, or course geometry.
4. A custom course's scenery is limited to the venue and skybox choices in Course Edit's
   Background menu. The buildings and landmarks of official courses are baked into each course
   and cannot be reused or replaced yet.
5. All sample pack replacements share 1 MiB of memory: about 80 seconds of cartridge music in
   total, or about 29 seconds for one Expansion Kit song.
6. Lua scripts are read-only: they cannot change the game yet.

## Logs

When a pack does nothing, the log usually says why. The log file is off by default.

**Turn it on** in one of two ways, then restart the game:

- Press **F1**, open **Dev Tools > General**, and tick **Write gdiffuser-run.log** under the
  logging developer gates.
- Or set the environment variable `GDX_LOG=1` before starting the game.

The log is written to `gdiffuser-run.log` next to the game executable. Search it for the prefix
of the kind of pack you are testing:

| Prefix | Comes from |
| --- | --- |
| `[workshop]` | Pack loading and reloading, texture packs, dump errors |
| `[seq-pack]` | Sequence packs |
| `[sample-pack]` | Sample packs |
| `[soundfont-pack]` | Soundfont packs |
| `[modelpacks]` | Model packs |
| `[lua` | Lua scripts and their `gdx.log` output |
