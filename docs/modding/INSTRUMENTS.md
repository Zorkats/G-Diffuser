# Instruments (soundfont packs)

> Replace the instrument set that a piece of note-based music plays with.

A **soundfont** is the set of instruments one piece of music uses: which recording each
instrument plays, its pitch range, and how its volume rises and fades. A soundfont pack replaces
one whole set.

This is an advanced channel. Read [Custom music](CUSTOM_MUSIC.md) first: most F-Zero X songs are
single recordings, and replacing those does not need a soundfont. Soundfonts matter for
note-based music, such as the Course Edit and Create Machine themes, which use more than a dozen
instruments each.

## What you need

- G-Diffuser, already set up and running.
- **Python 3.8 or later.** The soundfont tool needs nothing else.
- The **pack tools**. They are the Python scripts in the `tools/` folder of the G-Diffuser source
  code, and they are not part of the release download. On the
  [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser), click **Code**, then
  **Download ZIP**, and extract it anywhere. This guide calls that folder the **source folder**.
- WAV recordings for the instruments you want to change.

## Step 1: Dump the game's audio

1. Press **F1** and open **Workshop > Tools > Asset Dump**.
2. Tick **Audio Samples (WAV)**.
3. Click **Dump selected**, then **Open dump folder**.
4. Copy the whole `dump/` folder into the source folder, so that `dump/audio/` sits next to
   `tools/`.

## Step 2: Start a project

List the soundfont names, then start a project from the one you want:

```sh
python tools/gen_soundfont_pack.py init --names
python tools/gen_soundfont_pack.py init FONT_DDBGM_COURSE_EDITOR -o my-font
```

`init` writes a project folder with every stock instrument already filled in:

| File | What it is |
| --- | --- |
| `font.json` | The instrument list. This is what you edit. |
| `samples/` | The stock recordings, already converted. |

> **What `init` cannot copy.** The dump does not hold every stock detail, so check these before
> you edit:
>
> - **Expansion Kit recordings are silent placeholders.** They come from the 64DD disk and are
>   not in the dump. Replace each one you need with your own WAV.
> - **Volume shapes are reset.** Every instrument gets a simple default envelope, because the dump
>   does not hold the stock envelope points.
> - Recordings the dump does have are converted again, which loses a little quality.

## Step 3: Edit the instruments

Open `font.json`. Each entry in `instruments` is one instrument:

```json
{
  "index": 0,
  "rangeLo": 0,
  "rangeHi": 127,
  "adsrDecayIndex": 250,
  "envelope": "lead_env",
  "normal": { "sample": "lead", "tuning": 0.6890625 },
  "low": null,
  "high": null
}
```

To give an instrument a new sound:

1. Put your WAV in the project folder, for example `my-font/lead.wav`.
2. Add it under `samples`:

   ```json
   "samples": {
     "lead": { "wav": "lead.wav", "rateHz": 22050, "loop": { "start": 0, "end": 115700 } }
   }
   ```

   `loop` is optional. Leave it out for a sound that plays once.
3. Point the instrument's `normal` sound at it, as in the example above. `tuning` sets the pitch.

Volume shapes live under `envelopes`. Each point is `[time, level]`, and the last point must end
the shape. `[-1, 0]` means "hold here":

```json
"envelopes": { "lead_env": [[24, 32767], [-1, 0]] }
```

Rules the packer checks:

- **Keep the instrument count.** The music addresses instruments by number, so you change what
  each instrument sounds like, not how many there are.
- `low` and `high` sounds are needed only when the instrument's range does not reach the bottom
  (`rangeLo` above 0) or the top (`rangeHi` below 127).
- Each recording can be up to 8 MiB once decoded.

## Step 4: Build the pack

```sh
python tools/gen_soundfont_pack.py check my-font
python tools/gen_soundfont_pack.py pack my-font -o my-font.o2r --name "My Font" --author <your-name>
python tools/gen_soundfont_pack.py check my-font.o2r
```

`check` lists every problem at once.

## Step 5: Install it

1. Copy `my-font.o2r` into the `mods/` folder next to the game executable. **Workshop > Mods >
   Open mods folder** opens it for you.
2. On **Workshop > Mods**, tick **Enable soundfont packs**. It is off by default.
3. Make sure your pack's checkbox is ticked in the pack list.
4. Click **Reload packs**.

Notes already playing finish with the old instruments, and new notes use yours. Untick the switch
and click **Reload packs** to get the stock instruments back without restarting.

## How it behaves

- **All or nothing.** The game checks the whole soundfont before using it. If any part is wrong,
  it ignores the pack, keeps the stock instruments, and logs the reason as a `[soundfont-pack]`
  line.
- **Shared budget.** All soundfont packs together can use 8 MiB of memory. Past that, further
  packs are refused and logged. This budget is separate from the sample pack budget.

## If something goes wrong

**My instruments are silent.**
They still use the silent placeholders from `init`. Give them your own WAVs (step 3).

**The game ignores my pack.**
Search the log for `[soundfont-pack]`. The line names the problem. See
[Logs](../MODDING_GUIDE.md#logs).

**`init` says the dump was not found.**
Copy the game's `dump/` folder into the source folder (step 1).

## Quick reference

```sh
# Dump:    F1 > Workshop > Tools > Asset Dump > Audio Samples (WAV) > Dump selected
# Setup:   copy the game's dump/ folder into the source folder

python tools/gen_soundfont_pack.py init --names
python tools/gen_soundfont_pack.py init FONT_DDBGM_COURSE_EDITOR -o my-font
# Edit:    my-font/font.json, add your WAVs
python tools/gen_soundfont_pack.py check my-font
python tools/gen_soundfont_pack.py pack my-font -o my-font.o2r --name "My Font" --author <your-name>

# Install: copy my-font.o2r into mods/
# Enable:  F1 > Workshop > Mods > Enable soundfont packs > tick the pack > Reload packs
```
