# Machine models

> Replace a stock machine's 3D model with your own.

A model pack swaps the body of one or more of the 30 stock machines. You edit the model as an
ordinary OBJ file in any 3D tool, such as Blender. The packer converts it into the N64 format the
game draws.

The loop is: **dump** the stock models, **start a project** from the machine you want to replace,
**edit** the OBJ, **pack** it, and **install** it.

## What you need

- G-Diffuser, already set up and running.
- **Python 3.8 or later.** The model tool needs nothing else.
- The **pack tools**. They are the Python scripts in the `tools/` folder of the G-Diffuser source
  code, and they are not part of the release download. On the
  [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser), click **Code**, then
  **Download ZIP**, and extract it anywhere. This guide calls that folder the **source folder**.
- A 3D editor that imports and exports OBJ with **vertex colors**, such as Blender.

## Step 1: Dump the stock models

1. Press **F1** and open **Workshop > Tools > Asset Dump**.
2. Tick **Models (OBJ)**.
3. Click **Dump selected**, then **Open dump folder**.
4. Copy the whole `dump/` folder into the source folder, so that `dump/models/` sits next to
   `tools/`.

## Step 2: Start a project

From the source folder, run:

```sh
python tools/gen_model_pack.py init blue_falcon -o my-falcon
```

Replace `blue_falcon` with the machine you want. Names are lowercase with underscores:

```text
blue_falcon    golden_fox       wild_goose     fire_stingray   white_cat
red_gazelle    great_star       iron_tiger     deep_claw       twin_noritta
super_piranha  mighty_hurricane little_wyvern  space_angler    green_panther
black_bull     wild_boar        astro_robin    king_meteor     queen_meteor
wonder_wasp    hyper_speeder    death_anchor   crazy_bear      night_thunder
big_fang       mighty_typhoon   mad_wolf       sonic_phantom   blood_hawk
```

`init` creates a project folder that already holds the stock model, so you start from something
that works:

| File | What it is |
| --- | --- |
| `lod1.obj`, `lod1.mtl` | The stock model and its materials. This is what you edit. |
| `textures/` | The stock textures, as PNG. |
| `model.json` | The machine name, a `scale` factor, and which OBJ is each detail level. |

> If you would rather keep the dump somewhere else, add `--dump-dir <path-to-dump/models>`.

## Step 3: Edit the model

Open `lod1.obj` in your 3D editor, change it, and export it back over `lod1.obj`. These rules
matter:

1. **At most 106 triangles.** That is the most any stock machine uses. Quads are fine, and they
   count as two triangles. Faces with more than four corners are not allowed.
2. **Every vertex needs a color.** F-Zero X machines are colored per vertex, and the game does not
   use lighting or normals. Paint vertex colors and make sure your OBJ export includes them.
3. **Textures are small.** Each texture must have power-of-two sides and fit in 4 KiB. At the
   default format, 32x64 or 64x32 is the largest size. Paletted textures are not supported.
4. **Stay in range.** After the `scale` from `model.json` is applied, every position must fit the
   N64's 16-bit range. The packer refuses out-of-range models rather than squashing them.

Textured materials keep the game's **body recoloring**, so a machine still changes color the way
stock machines do. To give a material one flat color instead, put `--flat` at the end of its name
in the MTL (for example `cockpit--flat`). It uses the material's `Kd` color.

### Detail levels

The game draws simpler versions of each machine as it gets farther away: LOD 1 up close, down to
LOD 5 in the distance. `init` only creates `lod1.obj`. Any level you do not provide falls back to
the stock model, so a pack with only `lod1` is fine: your machine looks stock from far away. To
replace more levels, add `lod2.obj` to `lod5.obj` and list them under `lods` in `model.json`.

## Step 4: Build the pack

```sh
python tools/gen_model_pack.py pack my-falcon -o my-falcon.o2r --name "My Falcon" --author <your-name>
```

To check a project or a finished pack without writing anything:

```sh
python tools/gen_model_pack.py check my-falcon
python tools/gen_model_pack.py check my-falcon.o2r
```

Both commands list every problem at once. Lines that start with `warning:` do not stop the build.
Two of them are expected:

- `lodN is not packed; the stock model is used for that LOD` for each level you did not provide.
  The tool also mentions `lod6`, but the game only draws LOD 1 to 5, so a `lod6` is never used.
- `material 'none' has no texture` when a material has no texture: those faces use vertex colors
  only.

## Step 5: Install it

1. Copy `my-falcon.o2r` into the `mods/` folder next to the game executable. **Workshop > Mods >
   Open mods folder** opens it for you.
2. On **Workshop > Mods**, tick **Enable model packs**. It is off by default.
3. Make sure your pack's checkbox is ticked in the pack list.
4. Click **Reload packs**.

The new model appears the next time the game sets up the machines: on the machine select screen
or at the start of a race. Untick the switch and click **Reload packs** to get the stock models
back without restarting.

## What you can replace

| Can replace | Cannot replace yet |
| --- | --- |
| The body of the 30 stock machines | Machines built in Create Machine |
| | Create Machine parts (front, rear, wing) |
| | The debris of a crashed machine |
| | Course geometry and buildings |

## If something goes wrong

**`init` says the dump was not found.**
Run it from the source folder after copying `dump/` in (step 1), or pass `--dump-dir`.

**The packer reports too many triangles.**
Simplify the model to 106 triangles or fewer. `--allow-overbudget` turns the error into a warning,
at your own risk: no stock machine is heavier, so heavier models are untested.

**My machine is black or has no color.**
The OBJ was exported without vertex colors. Turn on vertex colors in the export settings.

**Nothing changed in the game.**
Check that **Enable model packs** is ticked and your pack is ticked, then go back to the machine
select screen.

**I need more detail.**
Turn on the log file and search it for `[modelpacks]`. Each applied or restored model is logged.
See [Logs](../MODDING_GUIDE.md#logs).

## Quick reference

```sh
# Dump:    F1 > Workshop > Tools > Asset Dump > Models (OBJ) > Dump selected
# Setup:   copy the game's dump/ folder into the source folder

python tools/gen_model_pack.py init blue_falcon -o my-falcon
# Edit:    my-falcon/lod1.obj (106 triangles max, vertex colors required)
python tools/gen_model_pack.py check my-falcon
python tools/gen_model_pack.py pack my-falcon -o my-falcon.o2r --name "My Falcon" --author <your-name>

# Install: copy my-falcon.o2r into mods/
# Enable:  F1 > Workshop > Mods > Enable model packs > tick the pack > Reload packs
```
