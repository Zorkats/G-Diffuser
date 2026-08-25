# G-Diffuser Modding Guide — Texture & Sequence Packs

This guide is for **modders**. It assumes you have never seen the source code. It walks you end to
end: **dump** the textures the game draws, **edit** them as ordinary PNGs, **pack** them into a
`.o2r` file, **install** the pack, and **reload** it live while the game runs.

By the end you will have a working texture pack in the `mods/` folder and see your art on screen.

---

## 1. What a texture pack is

A texture pack is a single file named `something.o2r`. Inside it is a set of replacement images,
one per game texture you want to override. When the pack is installed and enabled, the game draws
your images instead of the originals. Nothing about the original game files is modified — packs are
layered on top at runtime, and removing the `.o2r` restores stock rendering.

Each replacement is matched to an original texture by a **key** — a short text name like
`common_assets_compressed/aTitleLogoTex`. The key is the bridge between "the thing I saw on screen"
and "the file the game loads". You get the keys for free from the dump step below.

---

## 2. Before you start

You need:

- A working G-Diffuser build (the `G-Diffuser.exe` you already run).
- **Python 3.8+** with **Pillow**: `pip install Pillow`. This is only needed on your PC to build the
  pack; players do not need it.
- An image editor that can open and save PNG with alpha (GIMP, Photoshop, Krita, Aseprite, ...).

Two folders live **next to `G-Diffuser.exe`**. The game creates them on demand:

| Folder   | What it holds                                                        |
| -------- | -------------------------------------------------------------------- |
| `dump/`  | Dumped assets — PNG plus `manifest.tsv`                              |
| `mods/`  | Installed `.o2r` packs. Everything here is scanned at boot.           |

---

## 3. Step by step

### Step 1 — Dump the textures you want to replace

1. Launch the game and open the in-game menu.
2. Go to the **Workshop** tab → **Asset Dump**.
3. Leave **textures** ticked (all classes are on by default; untick the ones you do not need — each
   class runs as its own subprocess, so a broken class cannot abort the rest).
4. Run it. Assets are decoded straight from the extracted archive — you do **not** need to play
   through the screens you want to change.
5. Click **Open dump folder** to open `dump/` in your file browser.

Inside `dump/` you will find:

- `dump/<key>.png` — one PNG per texture, in sub-folders that mirror the key
  (e.g. `dump/common_assets_compressed/aTitleLogoTex.png`).
- `dump/manifest.tsv` — a table listing every dumped texture: **key, native width, native height,
  N64 format**. You do not edit this; the packer reads it in Step 3.

The keys mirror the game's own symbol names (`aTitleLogoTex`, `aKmhTex`, ...), so `manifest.tsv` is
the fastest way to find the one you want.

> **If you are following an older copy of this guide:** it told you to tick "Dump textures while
> playing" and to open a `dump/index.html` contact sheet. Both are gone. The play-as-you-go dumper was
> replaced by Asset Dump above, which is faster and complete — it does not depend on the game
> happening to walk past a texture. No contact sheet is generated any more.

> Tip: the textures are tiny — they are the raw N64 assets. Open them in an editor that can zoom
> without smoothing, or your first edit will look nothing like what ships.

`tools/gen_dump_all.py` does the same job from the command line if you prefer it, and writes the same
`manifest.tsv`.

### Step 2 — Edit the PNGs

Open the PNG for a texture you want to change and repaint it. Rules that matter:

- **Keep the same aspect and an integer multiple of the original size.** If the original is
  `160×6`, your replacement must be `160×6`, `320×12`, `480×18`, `640×24`, ... The packer enforces
  this and will tell you the allowed sizes if you get it wrong. A larger multiple = a hi-res
  replacement.
- **Keep alpha meaningful.** Many textures use their alpha channel for shape/transparency. Save as
  32-bit PNG with alpha.
- **Do not rename the file or move it out of its sub-folder.** The path *is* the key. Rename it and
  the game will not know which texture it replaces.

Delete any dumped PNGs you are **not** replacing — the packer simply ignores keys you leave out, but
a smaller folder is easier to work with. Textures you do not include keep their stock appearance.

### Step 3 — Pack it

From the repo (or anywhere the `tools/` script is reachable), run:

```
python tools/gen_texture_pack.py <path-to-your-edit-folder> my-pack.o2r --name "My Pack" --author you
```

Minimal form — this "just works" using sensible defaults:

```
python tools/gen_texture_pack.py dump/ my-pack.o2r
```

The packer:

- reads `manifest.tsv` from the folder to learn each texture's native size and format,
- re-encodes each of your PNGs into the exact N64 format the game expects (this is required — see
  §6),
- writes `my-pack.o2r`,
- and creates the pack metadata for you from `--name` / `--author` / `--version` if you did not
  supply a `workshop.json` (see §5).

**Validate before you ship** — this writes nothing and lists every problem it finds:

```
python tools/gen_texture_pack.py dump/ --check          # check an edit folder
python tools/gen_texture_pack.py my-pack.o2r --check    # check a built pack
```

If validation reports errors (wrong size, corrupt PNG, ...), fix them and re-run. The packer refuses
to write a pack while any error remains, and it reports **all** problems in one pass so you are not
fixing them one at a time.

### Step 4 — Install

Put `my-pack.o2r` into the `mods/` folder next to `G-Diffuser.exe`. Use **"Open mods folder"** in the
Workshop tab to get there quickly. That is the whole install.

### Step 5 — Enable and reload

In the Workshop tab:

1. Tick **"Texture packs"** (the master switch — off means stock rendering).
2. Your pack appears in the list. Make sure its checkbox is enabled.
3. Click **"Reload packs"**. This re-scans `mods/`, re-mounts packs, and clears the texture cache so
   your edits appear **without restarting the game**. The status line reports how many packs mounted
   and how many overrides are available.

Iterate: edit a PNG → re-run the packer → drop the new `.o2r` in `mods/` → **Reload packs**.

---

## 4. The key scheme

Every texture has a key. Where it comes from decides whether you can replace it:

| Key looks like                                   | Meaning                                | Replaceable?          |
| ------------------------------------------------ | -------------------------------------- | --------------------- |
| `common_assets_compressed/aTitleLogoTex`         | A **named** game asset                 | **Yes**               |
| `machine_custom_gfx/aLogoGoldenFoxTex`           | A **named** game asset                 | **Yes**               |
| `atlas/machine_custom_gfx/aTimerSymbolsTex/o256/RGBA16/8x16` | One **band** of a multi-tile atlas buffer | **Yes** (scheme 2) |
| `hash/97f5ac8c0cd4fb43`                          | An **unnamed** texture (content hash)  | **No** (dump-only)    |

Named keys are stable across runs and builds (that is what makes them safe to key on). `atlas/`
keys name one tile band of a buffer the game loads as a single tall strip: `o<byteOffset>` is the
band's byte offset inside that buffer, followed by the band's format and size. You never write
these by hand — the dump step emits one sliced PNG per band next to the whole-atlas PNG. Hash keys
are just a fingerprint of the pixels so unnamed textures still get dumped for reference — but the
game has no name to match them against at draw time, so a pack **cannot** override a `hash/...`
texture today. You can spot them in `manifest.tsv` by the `hash/` prefix on the key.

The build stamps a **key-scheme version** (currently `2`). If a future build renames symbols, the
version bumps and the menu flags older packs as out of date. You normally never touch this.

---

## 5. Pack metadata (`workshop.json`)

Every pack carries a small metadata file **inside** the archive named `workshop.json`:

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

| Field                | What it does                                                                 |
| -------------------- | --------------------------------------------------------------------------- |
| `name`               | Shown in the Workshop pack list.                                            |
| `version`            | Your pack's version. Free text.                                            |
| `author`             | Shown in the pack list.                                                     |
| `game_version`       | Target build. `us.rev0` is the current port. A different value is flagged. |
| `key_scheme_version` | The key scheme the pack was built against. A mismatch is flagged.          |
| `id`                 | Stable pack identity, used for load order and the disable list. Packs without one fall back to the archive basename. No commas. |
| `depends`            | Pack ids this pack needs. Missing ones produce a load-time warning only.   |
| `conflicts`          | Pack ids this pack clashes with. Loading both produces a warning only.     |
| `files`              | Integrity manifest (`{path, sha256}` per payload entry). Written by the packer, never by hand; `--check` re-verifies it. |

You do **not** have to write this file. If it is absent, the packer synthesizes one from the
`--name`, `--author`, `--version`, `--game-version`, `--key-scheme-version` and `--id` flags,
filling any you omit with safe defaults (`version 0.1`, `game_version us.rev0`,
`key_scheme_version 2`, and both the name and the id default to the output filename). If you *do*
provide a `workshop.json`, any flags you pass override the matching fields.

> Historical note: the file must be `workshop.json`, **not** `manifest.json` — the latter is a name
> reserved by the engine's archive loader and using it makes packs fail to mount. The packer handles
> this for you.

---

## 6. Why format matters (and what "native format" means)

The game decodes each texture using the **original N64 format** recorded in `manifest.tsv` (the `fmt`
column: `RGBA16`, `I4`, `IA8`, ...), *not* whatever format your PNG happens to be. The packer
therefore re-encodes your edited PNG back into that same native format automatically. You always
work in ordinary RGBA PNG; the packer does the conversion. You do not need to understand the N64
formats — just do not fight the size rule in §Step 2.

**Paletted formats work, at 1× only.** `CI4`/`CI8` textures are replaceable at native size: the
dump writes a `palette_key` column on those manifest rows naming a TLUT swatch PNG (an N×1 strip of
the original palette) dumped next to the texture, and the packer nearest-color quantizes your edit
back into that original palette. `--check` prints a per-texture quantization report so you can see
how much color error the quantization introduced. One exception stays dump-only: **CI atlas bands**
(`atlas/` keys with a `CI*` format) — a paletted band has no palette side-channel at runtime, so
the packer skips those with a warning.

---

## 7. Pack ordering and priority

- Load order is explicit: the Workshop menu lists packs in mount order and its **Up/Dn** buttons
  set that order (stored as the pack `id`s in `gEnhancements.Workshop.PackOrder`). Packs not in
  the list mount after it, alphabetical by filename.
- When two packs provide the **same key**, the later-mounted pack wins, so move the pack that
  should win further down the list.
- Disabling a pack in the menu adds its `id` (or filename, for packs without one) to a skip list;
  it is not mounted on the next **Reload packs** or boot. Re-enable and reload to bring it back.
  Boot and reload apply the same order and disable list.

---

## 8. Direct-key packs (advanced)

The default channel stores each replacement at `textures/pack/<key>`, and the engine consults it
texture by texture with guards (`hash/` keys skipped; atlas buffers matched per band via their
`atlas/` keys). There is a second, sharper channel: **direct-key shadowing**. Mod archives mount
after the base game archives and the loader is *last-wins*, so an entry stored at the **bare key** —
`<key>`, no `textures/pack/` prefix — replaces the base resource of that exact name for every
consumer. The mechanism is general (it reaches segment assets, models, arrays, and audio blobs
loaded by name); from this packer it applies to textures.

```
python tools/gen_texture_pack.py dump/ my-pack.o2r --name "My Pack" --direct-keys
```

Validation and encoding are identical; only the archive paths change, and `workshop.json` records
`"key_channel": "direct"` so `--check` and the Workshop tab can tell the two channels apart.

Before reaching for it:

- **It is unconditional once mounted.** A direct-key entry shadows the base resource everywhere —
  the default channel's per-key guards do not apply to this path. Test with `--check`
  first, and prefer the default channel unless you know you need this one.
- **`hash/` keys are rejected in direct mode** — they name no base resource, so they would shadow
  nothing.
- Atlas buffers sampled straight from emulated memory (§9) are not per-draw resource loads, so
  direct keys still do not reach them — the `atlas/` per-band keys of the default channel are the
  supported route there.
- Both channels share the same governance: **Workshop → "Texture packs" off mounts no packs at
  all**, direct-key packs included. The per-pack checkboxes apply on top of that.

---

## 9. What you CANNOT replace yet (current limitations)

Be aware of these before you plan a pack:

1. **Unnamed (`hash/...`) textures.** See §4 — no stable name to match at draw time.
2. **CI atlas bands.** Atlas bands in a paletted format (`atlas/` keys with `CI4`/`CI8`) are
   dump-only — see §6.
3. **Machine models: stock roster only.** Model packs (§16) replace the 30 stock machines'
   bodies; EK Create-Machine parts (front/rear/wing), fully custom roster entries, course
   geometry, and crashed-machine debris are not replaceable yet.
4. **Course scenery beyond venue/skybox.** A custom course's backdrop is its venue (ground +
   venue textures) plus its skybox — both per-course settings in Course Edit's Background menu
   (10 scenes x 8 skies stock; the port adds an 11th "Ending" scene under a checkbox). The
   buildings and landmarks on official courses are per-course model data baked into that course,
   not part of the venue/skybox system — they are not a palette you can pick from, and replacing
   or adding them is not possible yet.

**Atlas bands ARE replaceable now (key scheme 2).** Multi-tile "atlas" buffers (the timer-symbol
and speed-digit strips) dump as one PNG per band under
`atlas/<baseKey>/o<byteOffset>/<FMT>/<WxH>` next to the whole-atlas PNG, and the engine replaces
them band by band — a pack that provides only some bands leaves the rest stock. Old scheme-1 packs
that keyed on the base atlas key simply no longer match anything (they are inert — no garble, no
crash); re-dump and re-pack against scheme 2 to bring them back.

Everything else — named textures in a supported format (title art, logos, HUD elements, machine
graphics, ...) — is replaceable today.

---

## 10. Troubleshooting

**My pack does not show in the list.**
It must be a `*.o2r` file directly inside `mods/` (not a sub-folder). Click **Reload packs**.

**The list shows my pack but nothing changes on screen.**
- Is the **"Texture packs"** master switch on?
- Is the pack's own checkbox enabled?
- Did you click **Reload packs** after installing/editing?
- Is the texture actually replaceable? Check §9 — `hash/...` textures and CI atlas bands will not
  change. The Workshop tab shows an **"N override(s) available"** count; if it is `0`, none of your
  keys matched anything the game can override.

**Text turned into garbage after I added a font pack.**
That was the old whole-buffer atlas limitation. The engine now replaces atlas buffers per band
(§9), and old packs keyed on the base atlas key are inert rather than harmful — but re-dump and
re-build the pack against key scheme 2 so the band keys actually match.

**`gen_texture_pack.py` reports errors.**
Read them — each line names the offending key and the exact problem (wrong size with the list of
allowed sizes, unreadable PNG, unknown key, unsupported format). Fix them and re-run, or use
`--check` to iterate without writing a pack.

**Where are the logs?**
The game writes `gdiffuser-run.log` next to `G-Diffuser.exe` (and to stderr if you launched it from a
console). Workshop activity is prefixed `[workshop]` — reload results and dump-write failures show up
there. Grep that file when something silently does not work.

---

## 11. Quick reference

```
# Dump: Workshop tab -> Asset Dump -> run -> "Open dump folder"
# Look:  dump/manifest.tsv lists every key with its native size and format
# Edit:  repaint dump/<key>.png (keep size an integer multiple of the original)

# Build a pack (metadata synthesized from flags):
python tools/gen_texture_pack.py dump/ my-pack.o2r --name "My Pack" --author you

# Advanced: shadow base resources at their bare keys instead (§8):
python tools/gen_texture_pack.py dump/ my-pack.o2r --direct-keys

# Validate without writing (lists every problem):
python tools/gen_texture_pack.py dump/ --check
python tools/gen_texture_pack.py my-pack.o2r --check

# Install: copy my-pack.o2r into mods/  (Workshop tab -> "Open mods folder")
# Enable:  Workshop tab -> "Texture packs" on -> enable the pack -> "Reload packs"
```

---

## 12. Sequence packs (audio)

A pack can also override **music**: replace the game's sequenced tracks with your own. This is a
separate channel from textures with its own master switch.

**What you ship.** One raw sequence file per track, named `<name>.seq`, packed at
`audio/seq/<name>` inside the `.o2r`. The bytes are the **native N64 sequence format** (`.aseq` —
the same bytecode the game DMAs), not a container and not MIDI. Produce them with your own
audio tooling; this packer only validates names and wraps the bytes.

The 23 valid `<name>` values (list them any time with `python tools/gen_sequence_pack.py --names`):

```
guitar                sound_effects       ddbgm_mute_city       ddbgm_silence
ddbgm_sand_ocean      ddbgm_port_town     ddbgm_big_blue        ddbgm_devils_forest
ddbgm_red_canyon      ddbgm_sector        ddbgm_white_land      ddbgm_rainbow_road
ddbgm_new_03          ddbgm_new_02        ddbgm_new_01          ddbgm_new_04
ddbgm_title           ddbgm_select        ddbgm_option          ddbgm_deathrace
ddbgm_course_editor   ddbgm_machine_editor ddbgm_ead_demo
```

**Build, install, enable:**

```
python tools/gen_sequence_pack.py my-seq-dir/ my-music.o2r --name "My Music" --author you
python tools/gen_sequence_pack.py my-music.o2r --check    # validate without writing
```

Then drop `my-music.o2r` in `mods/`, tick **"Sequence packs"** in the Workshop tab (off by
default — off means stock music), and **Reload packs**.

Rules that differ from textures:

- **Size must fit the stock slot.** Each sequence loads into a heap slot sized for the original.
  An override larger than the stock sequence is ignored at runtime (stock loads instead) — keep
  your replacement at or under the original's byte size.
- **Match the stock byte format.** The file must be raw native `.aseq` bytes, exactly what the
  game DMAs for that track. In the extracted archives this is the raw payload of
  `ek/aAudioSeqDD<Name>` in `fzerox-disk.o2r`. A common extraction mistake is grabbing the wrong
  64DD sequence: `ddbgm_title`, `ddbgm_select`, and `ddbgm_option` are 48 bytes, while the race
  and menu DD BGM tracks (including `ddbgm_big_blue`) are 64 bytes. `tools/gen_sequence_pack.py`
  warns when the size does not match the stock sequence.
- **Use the same instrument bank, or ship a `.font` sidecar.** Each sequence slot loads the
  soundfont paired with that slot in the audio tables (for example `ddbgm_big_blue` loads
  `FONT_DDBGM_BIG_BLUE`). If you drop a sequence from a different slot into the Big Blue slot,
  the notes will ask Big Blue's bank for instruments that do not match, usually producing silence.
  To bring the original bank with the sequence, add a plain-text file named `<name>.font` next to
  your `<name>.seq`; its contents are the source font reference, e.g. `ddbgm_mute_city` or
  `FONT_DDBGM_MUTE_CITY`. The packer includes it as `audio/seq/<name>.font` and the loader uses
  that font for the slot. Without the sidecar, only same-bank sequence swaps are expected to sound.
- **Overrides apply at the next load.** A track already playing keeps its data until the next
  song/scene change (or a Reload packs) re-loads it. Sequences on the permanent heap are not
  retro-swapped mid-scene.
- Pack ordering, per-pack disable checkboxes, and `workshop.json` metadata work exactly like
  texture packs (§5, §7).

## 13. Sample packs (audio)

A pack can also override **individual instrument and sound-effect samples** — the ADPCM waveforms
the sequences play. This is a third channel with its own master switch ("Sample packs" in the
Workshop tab, off by default).

**What you ship.** One `.gsmp` file per replaced sample, packed at
`audio/sample/<bank>__0x<offset>__<size>` inside the `.o2r`, where `<bank>` is the sample-bank
name, `<offset>` the sample's byte offset within that bank (uppercase hex), and `<size>` the
**stock** sample's encoded byte size (decimal). The dumper emits keys in exactly this shape, so a
dumped sample's key is a valid override key verbatim. Produce the `.gsmp` containers with
`python tools/gen_sample_pack.py` — it encodes your WAV to ADPCM, wraps it in the container, and
validates names and sizes.

The 25 valid `<bank>` values (the `SampleBankId` enum names, exactly as the dump emits them):

```
SAMPLE_SOUND_EFFECTS   SAMPLE_BGM              SAMPLE_GUITAR         SAMPLE_DD_SOUND_EFFECTS
SAMPLE_DDBGM_MUTE_CITY SAMPLE_DDBGM_SILENCE    SAMPLE_DDBGM_SAND_OCEAN SAMPLE_DDBGM_PORT_TOWN
SAMPLE_DDBGM_BIG_BLUE  SAMPLE_DDBGM_DEVILS_FOREST SAMPLE_DDBGM_RED_CANYON SAMPLE_DDBGM_SECTOR
SAMPLE_DDBGM_WHITE_LAND SAMPLE_DDBGM_RAINBOW_ROAD SAMPLE_DDBGM_NEW_03   SAMPLE_DDBGM_NEW_02
SAMPLE_DDBGM_NEW_01    SAMPLE_DDBGM_NEW_04     SAMPLE_DDBGM_TITLE      SAMPLE_DDBGM_SELECT
SAMPLE_DDBGM_OPTION    SAMPLE_DDBGM_DEATHRACE  SAMPLE_DDBGM_COURSE_EDITOR
SAMPLE_DDBGM_MACHINE_EDITOR SAMPLE_DDBGM_EAD_DEMO
```

Rules that differ from sequence packs:

- **Replacement may differ in length.** Samples are copied into a fresh arena allocation, not the
  stock heap slot, so the override is not capped by the stock size. (Total arena budget across all
  sample overrides: 1 MiB; past that, further overrides are refused and logged.)
- **Identity-checked.** The key's `<size>` must match the stock sample's size at runtime, and the
  container carries a CRC-32 of its payload — a stale or corrupt override is ignored (stock sample
  plays) with a `[sample-pack]` line in the log.
- **Overrides apply at the next font conversion.** A sample already playing keeps its data until
  the next scene/song load (or a Reload packs) re-converts the owning font. Samples are
  ADPCM-only; loop points and predictor state ship inside the container.
- Pack ordering, per-pack disable checkboxes, and `workshop.json` metadata work exactly like
  texture packs (§5, §7).

## 14. Soundfont packs (audio)

A pack can replace a **whole soundfont bank** — the instrument table a sequence picks its
sounds from — not just individual samples. Fourth channel, own master switch ("Soundfont packs"
in the Workshop tab, off by default).

**What you ship.** One `audio/font/<FONTNAME>` file inside the `.o2r`, where `<FONTNAME>` is one
of the 23 font names (the `FontId` enum identifiers, verbatim):

```
FONT_GUITAR             FONT_SOUND_EFFECTS      FONT_DDBGM_MUTE_CITY  FONT_DDBGM_SILENCE
FONT_DDBGM_SAND_OCEAN   FONT_DDBGM_PORT_TOWN    FONT_DDBGM_BIG_BLUE   FONT_DDBGM_DEVILS_FOREST
FONT_DDBGM_RED_CANYON   FONT_DDBGM_SECTOR       FONT_DDBGM_WHITE_LAND FONT_DDBGM_RAINBOW_ROAD
FONT_DDBGM_NEW_03       FONT_DDBGM_NEW_02       FONT_DDBGM_NEW_01     FONT_DDBGM_NEW_04
FONT_DDBGM_TITLE        FONT_DDBGM_SELECT       FONT_DDBGM_OPTION     FONT_DDBGM_DEATHRACE
FONT_DDBGM_COURSE_EDITOR FONT_DDBGM_MACHINE_EDITOR FONT_DDBGM_EAD_DEMO
```

The file is a **GFT1 container**: a self-contained description of the whole bank — every
instrument (sample references, envelope, ranges, tuning), every envelope curve, and every sample
as an embedded `.gsmp` blob (the same container §13 uses). Produce packs with
`python tools/gen_soundfont_pack.py`:

```
python tools/gen_soundfont_pack.py init FONT_DDBGM_BIG_BLUE -o myfont/   # editable project
python tools/gen_soundfont_pack.py pack myfont/ -o myfont.o2r            # validate + pack
python tools/gen_soundfont_pack.py check myfont.o2r                      # re-validate
```

`init` pre-fills every stock instrument from the dump so you start from a working bank and edit
`font.json` + drop in your own WAVs. Note: sample banks that only exist on the 64DD disk have no
dumped waveforms, so `init` writes short silent placeholders for those samples — replace them with
your own WAVs (banks shared with the cartridge, e.g. FONT_GUITAR, are re-encoded for real).

Rules that differ from sample packs:

- **Whole bank, all or nothing.** The container is validated completely — header, instrument /
  envelope / sample reference graph, envelope curves, every embedded sample, CRC — *before*
  anything changes. A pack that fails any check is ignored wholesale (stock bank keeps playing)
  with a `[soundfont-pack]` line in the log naming the reason. There is no partial application.
- **Counts are locked.** Instruments replace contents, not dimensions: the overlay must have
  exactly the stock instrument count for that font (sequences are compiled against stock program
  numbers). All 23 fonts have zero drums and zero sound-effects entries.
- **Swaps are note-safe.** The new bank is published at the font's load/convert point or at a
  pack Reload, between audio ticks. A note already sounding keeps its old sample pointers until
  it ends; new notes use the new bank. Disabling the switch (or removing the pack) plus a Reload
  restores the stock bank without a restart.
- **Budget.** Each embedded sample may decode to at most 8 MiB, and all soundfont overrides share
  an 8 MiB arena (separate from the 1 MiB sample-pack budget). Past that, further overlays are
  refused and logged.
- Pack ordering, per-pack disable checkboxes, and `workshop.json` metadata work exactly like
  texture packs (§5, §7); use `pack_type: "soundfont"` (the packer writes it for you).

## 15. Lua script packs

A pack can ship **Lua scripts** that react to race events — the scripting channel, with its own
master switch ("Enable Lua script packs" in the Workshop tab, off by default). Scripts are
sandboxed: no filesystem, no OS access, no loading other code, a per-callback instruction budget,
and a per-script memory cap. A script that errors or exceeds a budget is auto-disabled until the
next Reload packs; it never affects the game or the other scripts.

**What you ship.** `scripts/<name>.lua` entries inside the `.o2r` (alongside `workshop.json`).
Every pack's scripts run, in pack order (§7); two packs may both ship a `scripts/hud.lua` — each
runs in its own sandbox. Load and Reload packs re-scan; toggling the switch off tears everything
down. The Workshop tab's "Installed scripts" list shows each script's status, and load/runtime
errors appear there in red and in the log as `[lua] <pack>/<script>: ...`.

**API v1 (read-only).** Everything lives in the global `gdx` table:

```lua
gdx.log(msg)              -- write to the log, prefixed with your pack/script name
gdx.frame()               -- current frame counter
gdx.mode()                -- current game mode (number)
gdx.racerCount()          -- grid size (30)
gdx.racer(i)              -- per-call snapshot table for racer i (nil if out of range):
                          -- { id, speed, energy, maxEnergy, lap, lapsCompleted, position,
                          --   raceTime (ms), finished, crashed }

function gdx.onFrame() end                     -- every rendered frame (keep it cheap)
function gdx.onRaceStart() end                 -- once, at GO
function gdx.onLap(racerId, lap) end           -- lap is the post-increment counter (finished lap = lap - 1)
function gdx.onFinish(racerId, position, raceTimeMs) end
```

Snapshots are per-call: never keep a `gdx.racer()` table across frames. Racer ids 0..N-1 are the
human players; the rest are CPU.

**Limits.** Per callback: 1,000,000 Lua VM instructions (a script that trips this is disabled,
not the game). Per script: 8 MiB of memory, own interpreter state. Available libraries: base
(minus `load`/`dofile`/`require`/`collectgarbage`), `string`, `table`, `math`, `utf8`. No `io`,
`os`, `package`, `debug`, or `coroutine` in v1, and scripts cannot write game state yet — both
arrive in later versions.

A complete commented example ships at `docs/examples/lua/lap_timer.lua` (logs every racer's lap
times). To try it: zip a folder containing `workshop.json` and `scripts/lap_timer.lua` into
`mods/lap_timer.o2r`, enable the checkbox, and watch the log.

## 16. Model packs

A pack can replace a **stock machine's 3D model** with a custom one, baked from an OBJ you edit
in any modeling tool. Fifth Workshop channel, own master switch ("Enable model packs", off by
default).

**What you ship.** For each machine and detail level you replace, one baked display list at
`models/pack/machine/<machine>/lod<N>` inside the `.o2r`, where `<machine>` is the stock machine
name in lowercase with underscores (`blue_falcon`, `wild_goose`, `fire_stingray`, …) and N is
1..5 (1 = highest detail, shown up close; 5 = farthest). LODs you don't ship stay stock — a
`lod1`-only pack is legal, the machine just reverts to stock at distance. The display lists
reference their vertex data and textures inside the same pack by hash, so a model pack is fully
self-contained.

**The baker does the hard part.** Models must be F3DEX2 display lists in the game's exact vertex
format — `tools/gen_model_pack.py` converts OBJ+MTL into them:

```
python tools/gen_model_pack.py init blue_falcon -o myship/      # editable project from the stock dump
python tools/gen_model_pack.py pack myship/ -o myship.o2r       # validate + bake + pack
python tools/gen_model_pack.py check myship.o2r                 # re-validate a packed file
```

`init` starts you from the real stock model (dumped OBJ + textures), so a recolor-geometry or a
from-scratch replacement both begin from something that works. Edit `lod1.obj` (vertex colors
are mandatory — F-Zero X machines are vertex-colored; normals/lighting are not used), keep or
replace the textures, then `pack`.

Rules the baker enforces (hard failures refuse the pack; `check` lists every problem at once):

- **Poly budget:** at most **106 triangles per LOD** — the highest stock machine count
  (`--allow-overbudget` demotes this to a warning, at your own risk).
- **Vertex format:** positions must fit signed 16-bit after your chosen `scale` (in
  `model.json`); out-of-range is rejected, never silently clamped. Power-of-two textures, at most
  4 KiB per tile (RGBA16/RGBA32 only — no paletted).
- **Triangles/quads only;** vertex colors on every used vertex; one texture per material name.
- **Recolors keep working:** the default material template shades through the game's body-color
  register, so opponent-color features and machine recolors tint your model exactly like stock.
  Name a material with a `--flat` suffix to opt out (flat color from the MTL instead).

**Runtime semantics.** Overrides apply when a machine's model lists are (re)registered — entering
the machine-select screen or a race — and on Reload packs. Turning the switch off plus a Reload
restores the stock models without a restart; the log shows `[modelpacks]` lines for each applied
or restored override. Crashed-machine debris, EK Create-Machine parts, and course geometry are
not affected (see §9).

## 17. Post-process shaders

You can replace the built-in CRT/scanline filters with your own fullscreen shader. Shaders live in a
`shaders/` folder **next to `G-Diffuser.exe`**. The menu's **Settings → Shaders → Shader preset** lists
one entry per shader file found for the active backend.

**Two backends, two file types:**

- **OpenGL:** write a `.glsl` file.
- **DirectX 11:** write a `.hlsl` file.

No cross-compilation is supported; the file must match the renderer you are running. Switching
backends in `gdiffuser.cfg.json` (`Window.Backend.Id`) requires a matching shader file for the new
backend.

**Contract.** The backend gives you a fullscreen triangle and a small fixed set of inputs. You only
write the per-pixel body; the backend injects the surrounding boilerplate.

| Symbol | OpenGL `.glsl` | DirectX 11 `.hlsl` |
| ------ | -------------- | ------------------ |
| Input UV | `vUV` | `input.vUV` |
| Source texture | `sampler2D uTex` + `texture(uTex, vUV)` | `Texture2D uTex` + `SamplerState uTexSampler` + `uTex.Sample(uTexSampler, input.vUV)` |
| Source size (native N64 res) | `uniform vec2 uSrcSize;` | `float2 uSrcSize;` in `cbuffer PostCB : register(b0)` |
| Output size | `uniform vec2 uOutSize;` | `float2 uOutSize;` in same cbuffer |
| Fragment output | `gdxFragColor` | `float4 PSMain(PSInput input) : SV_Target { ... }` |

The backend injects the correct `#version` for the GL profile, so do **not** put your own
`#version` directive in a `.glsl` file. The D3D11 backend compiles against shader model 4.0.

**Hot reload.** Edit and save the file while the game is running; the backend recompiles it the next
time the menu selection is active and the file's modification time has changed. A compile error logs
to the console and the game falls back to presenting the unfiltered frame.

**Example.** A minimal scanline shader ships in both formats at `docs/examples/shaders/`:

- `example-scanlines.glsl` for OpenGL
- `example-scanlines.hlsl` for DirectX 11

Copy the one matching your backend into `shaders/` next to the exe and it appears in the menu as
**example-scanlines**.

## 18. Post-process pipelines (multi-pass `.slangp`) and `.slang` shaders

You can chain several fullscreen shaders into a **pipeline**, or drop a single
`.slang` shader into `shaders/` to use it as a one-pass preset. The menu's
**Settings → Shaders → Shader preset** lists the built-in **Off /
Scanlines / CRT** modes, every `.slangp` preset found **anywhere under
`shaders/`** (nested folders display as `category/preset`), and one entry per
loose `.slang` file in `shaders/`. The `shaders/include/` folder is skipped: it
holds shared headers that pass shaders pull in with `#include`.

**How a pipeline differs from a single shader.** A single-file custom shader
(§17) is one pass: the frame is downsampled to the N64-native resolution, the
shader runs, and the result is drawn. A pipeline can run any number of passes,
each with its own source scale, output scale, linear/nearest sampling, and
clamp/wrap addressing. The last pass always renders to the final output size.
A loose `.slang` file in `shaders/` behaves like a one-pass pipeline at source
scale.

**Where presets and shaders live.** Your own `.slangp` presets conventionally go
in `shaders/pipelines/`, and the pass shaders they reference live in the **same
folder**. A preset named `my-effect.slangp` with `shader0 = "scanlines"` resolves
to `shaders/pipelines/scanlines.slang` if it exists, otherwise to
`shaders/pipelines/scanlines.glsl` on OpenGL or `shaders/pipelines/scanlines.hlsl`
on DirectX 11. You can write the shader key with or without an extension
(`.slang`, `.glsl`, and `.hlsl` are stripped).

Presets also work from any other subfolder of `shaders/` — pass paths are always
resolved relative to the preset's own folder, so a whole pack tree (for example
the `shaders/slang-shaders/` folder the in-app downloader creates, with its
`crt/`, `scanlines/`, ... categories) works as-is. Nested presets appear in the
menu as `category/preset`.

Loose `.slang` files placed directly in `shaders/` appear in the menu with a
`(.slang)` suffix and run as a single pass with the same builtin contract as a
pipeline pass.

**Supported keys this slice.** Keys beyond these are parsed but are no-ops until
later slices.

| Key | Meaning |
| --- | --- |
| `shaders` | Number of passes (1..64). |
| `shaderN` | Path/stem of pass N's shader file, relative to the preset folder. |
| `filter_linearN` | `true` for GL/D3D11 linear sampling, `false` for nearest. Default `true`. |
| `wrap_modeN` | `clamp_to_edge`, `clamp_to_border`, `repeat`, or `mirrored_repeat`. Default `clamp_to_edge`. |
| `scale_typeN` | How to interpret `scaleN`: `source` (multiply input size), `absolute` (fixed pixels), or `viewport` (multiply final output size). Default `source`. |
| `scale_type_xN` / `scale_type_yN` | Per-axis overrides for `scale_typeN`. |
| `scaleN` | Scale factor for both axes. Default `1.0`. |
| `scale_xN` / `scale_yN` | Per-axis scale overrides. |
| `aliasN` | Reserved name for pass N's output in other passes' shaders. Parsed; aliases are not resolved yet, reference passes by index (`PassOutputN`). |
| `float_framebufferN` | Render pass N into a 16-bit-float target instead of 8-bit UNORM. Use for HDR-style pipelines. |
| `srgb_framebufferN` | Render pass N into an sRGB target. |
| `frame_count_modN` | Run pass N only on frames where `FrameCount % N == 0`. On skipped frames downstream passes sample the pass output from the last run. |
| `feedback_passN` | Keep pass N's output across frames so later passes can sample it as `PassFeedbackN` next frame. |
| `textures` block | LUT textures exposed to shaders as `User0`, `User1`, ... in declaration order. |

Any top-level key that is not one of the names above is treated as a **shader
parameter override**. Pass shaders declare parameters with `#pragma parameter`,
and the preset can override the default value:

```
shaders = 1
shader0 = "example-scanlines"
scanline_brightness = "0.65"
```

**Inheritance.** A preset can start with `#reference "other.slangp"` to load
another preset first and then override any of its keys. References are resolved
relative to the preset folder and chained up to four levels deep.

**The `.slang` contract.** A `.slang` pass is translated through glslang and
SPIRV-Cross to the active backend, so one file works on both OpenGL and DirectX
11. Write the fragment stage (or omit `#pragma stage` and write the body
directly). The translator exposes these builtins:

| Symbol | Meaning |
| --- | --- |
| `Source` / `Original` | Source texture (`sampler2D`). |
| `SourceSize` / `OriginalSize` | `vec2` resolution of the input texture. |
| `OutputSize` / `FinalViewportSize` | `vec2` resolution of the output. |
| `FrameCount` | `uint` frame counter (per-pass `frame_count_modN` does not change this value; it gates whether the pass runs). |
| `MVP` | `mat4` model-view-projection. |
| `vTexCoord` / `TexCoord` | `vec2` input UV. |
| `FragColor` | `vec4` fragment output. |
| `OriginalHistory1`, `OriginalHistory2`, ... | The source frame as it was 1, 2, ... frames ago (native resolution). |
| `PassOutput0`, `PassOutput1`, ... | This frame's output of pass N. Only valid for passes before the current one. |
| `PassFeedback0`, `PassFeedback1`, ... | Last frame's output of pass N; requires `feedback_passN = "true"` on that pass. |
| `User0`, `User1`, ... | LUT textures declared in the preset's `textures` key. |

Declare tunables with `#pragma parameter`:

```
#pragma parameter scanline_brightness "Scanline brightness" 0.80 0.0 1.0 0.01
```

The translator strips declarations of these builtins and the `#pragma parameter`
lines, then maps the remaining body to the host backend. Do not add your own
`#version` directive.

**Per-pass shader contract for `.glsl`/`.hlsl` passes.** Non-slang passes still
reuse the exact same single-file contract as §17. The backend injects the vertex
shader and the input/output uniforms; you only write the fragment body.

| Symbol | OpenGL `.glsl` | DirectX 11 `.hlsl` |
| ------ | -------------- | ------------------ |
| Input UV | `vUV` | `input.vUV` |
| Source texture | `sampler2D uTex` + `texture(uTex, vUV)` | `Texture2D uTex` + `SamplerState uTexSampler` + `uTex.Sample(uTexSampler, input.vUV)` |
| Source size | `uniform vec2 uSrcSize;` | `float2 uSrcSize;` in `cbuffer PostCB : register(b0)` |
| Output size | `uniform vec2 uOutSize;` | `float2 uOutSize;` in same cbuffer |
| Fragment output | `gdxFragColor` | `float4 PSMain(PSInput input) : SV_Target { ... }` |

Pass 0 receives the game framebuffer as input at N64-native resolution
(`SourceSize` / `uSrcSize` = `mNativeDimensions`). Pass N receives the output of
pass N-1 as input, so the source size is the size you configured for the
previous pass.

**Hot reload.** Edit and save the `.slangp`, any pass shader, or a loose `.slang`
file while the game is running; the engine re-parses and recompiles the pipeline
the next frame. Shared headers under `shaders/include/` are **not** watched, so
after editing one, touch the pass file (or restart) to force a rebuild. Set the environment variable `GDX_DUMP_SLANG_TRANSLATION` to
write the translated GLSL/HLSL source to `logs/slang-<stem>-<backend>.txt` for
debugging. A compile error logs to the console and the frame falls back to the
unfiltered source.

**Example (Slice 2 `.slang`).** A one-pass `.slangp` preset ships at
`docs/examples/shaders/pipelines/example-scanlines.slangp`:

```
shaders = 1

shader0 = "example-scanlines"
filter_linear0 = "false"
wrap_mode0 = "clamp_to_edge"
scale_type0 = "source"
scale0 = "1.0"
```

Its pass shader is `docs/examples/shaders/pipelines/example-scanlines.slang`:

```
#pragma parameter scanline_brightness "Scanline brightness" 0.80 0.0 1.0 0.01

void main() {
    vec3 color = texture(Source, vTexCoord).rgb;
    float scan = scanline_brightness + (1.0 - scanline_brightness) *
                 cos(6.2831853 * (fract(vTexCoord.y * SourceSize.y) - 0.5));
    FragColor = vec4(color * scan, 1.0);
}
```

Copy both files into `shaders/pipelines/` next to the exe, then select
**example-scanlines** in the menu.

**Example (Slice 1 `.glsl`/`.hlsl`).** A two-pass preset using the older
backend-specific contract ships at
`docs/examples/shaders/pipelines/example-2pass.slangp`:

```
shaders = 2

shader0 = "example-scanlines"
filter_linear0 = "false"
scale_type0 = "source"
scale0 = "1.0"

shader1 = "satpixie-crt"
filter_linear1 = "true"
scale_type1 = "viewport"
scale1 = "1.0"
```

Copy `example-2pass.slangp` and the matching `example-scanlines.glsl`,
`example-scanlines.hlsl`, `satpixie-crt.glsl`, and `satpixie-crt.hlsl` into
`shaders/pipelines/` next to the exe, then select **example-2pass** in the menu.

**Runtime parameters.** When a `.slangp` preset or loose `.slang` shader is
active, every `#pragma parameter` found across its pass shaders appears as a
slider in **Settings → Shaders → Shader parameters**. The slider label is the
quoted description from the pragma (`#pragma parameter scanline_brightness
"Scanline brightness" 0.80 0.0 1.0 0.01` shows as **Scanline brightness**);
the tooltip shows the raw name, default, and range. **Reset parameters to
defaults** discards your edits. Slider values persist to
`gEnhancements.Graphics.PipelineParam.<preset-stem>.<parameter>` and take
precedence over the preset's own override, which in turn overrides the shader's
declared default. **Save custom preset** writes a new `.slangp` into
`shaders/pipelines/` that `#reference`s the active preset and bakes in your
slider values, RetroArch simple-preset style.

**Community shader pack (in-app download).** The **Download slang-shaders
pack** button on the Shaders page fetches the upstream
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) collection
from GitHub and extracts it into `shaders/slang-shaders/`. The pack has no
single license — each shader file carries its own header (MIT, GPLv2, GPLv3,
public domain) — which is exactly why G-Diffuser downloads it on your request
instead of bundling it. After the download finishes, the pack's presets appear
in the dropdown as `slang-shaders/<category>/<name>`.

**Current limitations.** This is Slice 3: history (`OriginalHistoryN`), feedback
(`PassFeedbackN` / `feedback_passN`), per-pass float/sRGB targets,
`frame_count_modN`, LUT textures, and runtime parameter editing all work.
Not yet supported: compute/geometry/tessellation stages and alias resolution by
name.
