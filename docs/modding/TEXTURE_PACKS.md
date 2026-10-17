# Texture packs

> Repaint the game's textures and see them in the game without touching the original files.

A texture pack is one `.o2r` file in the `mods/` folder. It holds replacement images for the game
textures you want to change. Textures you leave out keep their stock look, and deleting the pack
restores stock rendering.

You do not need to read any source code. The whole loop is: **dump** the textures, **edit** them
as ordinary PNG files, **pack** them, **install** the pack, and **reload** it while the game runs.

## What you need

- G-Diffuser, already set up and running.
- **Python 3.8 or later**, with **Pillow** installed: `pip install Pillow`. Only you need these to
  build the pack. Players who install your pack do not.
- The **pack tools**. They are the Python scripts in the `tools/` folder of the G-Diffuser source
  code, and they are not part of the release download. On the
  [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser), click **Code**, then
  **Download ZIP**, and extract it anywhere.
- An image editor that saves PNG with transparency, such as GIMP, Krita, Photoshop, or Aseprite.

Press **F1** in the game to open the menu. Everything below happens on two pages of the
**Workshop** tab: **Tools** (dumping) and **Mods** (installing and reloading).

## Step 1: Dump the textures

1. Open **Workshop > Tools > Asset Dump**.
2. Make sure the **textures** class is ticked. You can untick the other classes to finish faster.
3. Click **Dump selected**. The game decodes every texture straight from its asset archive, so you
   do not need to play through the screens you want to change.
4. Click **Open dump folder**.

The dump folder is `dump/`, next to the game executable. Inside it:

| File | What it is |
| --- | --- |
| `dump/<key>.png` | One PNG per texture, in subfolders that mirror the key. For example, `dump/common_assets_compressed/aTitleLogoTex.png`. |
| `dump/manifest.tsv` | A table of every dumped texture: its key, native width, native height, and N64 format. You never edit it. The packer reads it. |

The **key** is the texture's name, for example `common_assets_compressed/aTitleLogoTex`. Keys
follow the game's own symbol names, so searching `manifest.tsv` for a word like `Logo` or `Kmh` is
the fastest way to find a texture.

> **Tip:** these are the original N64 textures, so they are tiny. Use an editor zoom that does not
> smooth pixels, or your edit will not look like what the game draws.

## Step 2: Edit the PNG files

Open a PNG and repaint it. Three rules matter:

1. **Keep the same shape, at a whole-number multiple of the original size.** If the original is
   `160x6`, your replacement can be `160x6`, `320x12`, `480x18`, `640x24`, and so on. A bigger
   multiple gives you a high-resolution replacement. The packer rejects any other size and lists
   the sizes it accepts.
2. **Keep the transparency.** Many textures use their alpha channel for their shape. Save as
   32-bit PNG with alpha.
3. **Do not rename or move the file.** Its path inside the dump folder is its key. A renamed file
   no longer matches any texture.

Copy the textures you change into a separate folder, keeping their subfolders, and copy
`manifest.tsv` next to them. A small folder is easier to work with, and textures you leave out
keep their stock look. For a paletted texture (format `CI4` or `CI8`), also copy the palette PNG
named in that row's `palette_key` column.

## Step 3: Build the pack

Open a terminal in the folder where you extracted the source code and run:

```sh
python tools/gen_texture_pack.py <your-edit-folder> my-pack.o2r --name "My Pack" --author <your-name>
```

Replace `<your-edit-folder>` with the folder from step 2, and `<your-name>` with your name.

The packer reads `manifest.tsv`, converts each PNG back into the texture's original N64 format,
and writes `my-pack.o2r`. It also writes the pack's metadata for you. To check everything without
writing a pack, add `--check`:

```sh
python tools/gen_texture_pack.py <your-edit-folder> --check
python tools/gen_texture_pack.py my-pack.o2r --check
```

`--check` lists every problem in one pass: wrong sizes, unreadable PNG files, unknown keys, and
unsupported formats. The packer refuses to write a pack while any problem remains.

## Step 4: Install and reload

1. Copy `my-pack.o2r` into the `mods/` folder next to the game executable. **Workshop > Mods >
   Open mods folder** opens it for you.
2. On **Workshop > Mods**, tick **Enable texture packs**. It is off by default.
3. Find your pack in the pack list and make sure its checkbox is ticked.
4. Click **Reload packs**.

Reload packs re-scans `mods/` and clears the texture cache, so your art appears **without
restarting the game**. The status line under the button shows how many packs mounted and how many
overrides are available.

To iterate: edit a PNG, rebuild the pack, copy it into `mods/` again, and click **Reload packs**.

## What you can replace

| Key looks like | What it is | Replaceable? |
| --- | --- | --- |
| `common_assets_compressed/aTitleLogoTex` | A named texture | Yes |
| `atlas/machine_custom_gfx/aTimerSymbolsTex/o256/RGBA16/8x16` | One band of a multi-tile strip, such as the timer digits | Yes |
| `hash/97f5ac8c0cd4fb43` | An unnamed texture, identified only by its pixels | No |

- **Named textures** cover title art, logos, HUD elements, machine graphics, and most of the
  game.
- **Atlas bands** are strips the game loads as one tall image. The dump writes one PNG per band
  next to the whole strip. Replace the bands you want, and the rest stay stock.
- **`hash/` textures** are dumped for reference only. The game has no name to match them against
  while drawing.
- **Paletted textures** (`CI4` and `CI8` formats) work at their original size only. The packer
  maps your colors back onto the original palette and `--check` reports how far the colors moved.
  Paletted atlas bands cannot be replaced.

For the full key scheme, metadata, load order, and the advanced direct-key mode, see the
[modding reference](../MODDING_GUIDE.md#reference).

## If something goes wrong

**My pack is not in the list.**
The file must end in `.o2r` and sit directly inside `mods/`, not in a subfolder. Click
**Reload packs**.

**My pack is in the list, but nothing changes.**
Check these in order:

1. **Enable texture packs** is ticked.
2. Your pack's checkbox is ticked.
3. You clicked **Reload packs** after copying the pack.
4. The status line reports more than `0` overrides. If it reports `0`, none of your keys match a
   texture the game can replace. Check the table above.

**The packer reports errors.**
Each line names the texture and the exact problem, including the allowed sizes. Fix them and run
`--check` again.

**Text turned into garbage after installing an old font pack.**
That pack was built for an older key scheme. Dump again and rebuild it with the current packer.

**I need more detail.**
Turn on the log file, reproduce the problem, and search the log for `[workshop]`. See
[Logs](../MODDING_GUIDE.md#logs).

## Quick reference

```sh
# Dump:    F1 > Workshop > Tools > Asset Dump > Dump selected > Open dump folder
# Find:    dump/manifest.tsv lists every key with its native size and format
# Edit:    repaint dump/<key>.png at a whole-number multiple of its size

# Build
python tools/gen_texture_pack.py <your-edit-folder> my-pack.o2r --name "My Pack" --author <your-name>

# Validate without writing
python tools/gen_texture_pack.py <your-edit-folder> --check
python tools/gen_texture_pack.py my-pack.o2r --check

# Install: copy my-pack.o2r into mods/
# Enable:  F1 > Workshop > Mods > Enable texture packs > tick the pack > Reload packs
```
