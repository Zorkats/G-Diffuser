# Sound effects and voices

> Replace a sound effect or an announcer line with your own recording.

Every sound in F-Zero X is a short recording, called a **sample**. A sample pack replaces samples
by name: you give your WAV the same name as the stock recording, and the game plays yours instead.
Native music recordings use this channel too. For complete songs, prefer
[streamed custom music](CUSTOM_MUSIC.md), which does not need Python or the sample-pack budget.

The loop is: **dump** the game's audio, **find** the sound, **export** your recording with the same
name, **pack** it, and **install** it.

> **Important:** the game never reads WAV files from a pack directly. Zipping your WAVs and
> renaming the file to `.o2r` does not work. Always build the pack with the packer in step 4.

## What you need

- G-Diffuser, already set up and running.
- **Python 3.8 or later.** The sample tool needs nothing else.
- The **pack tools**. They are the Python scripts in the `tools/` folder of the G-Diffuser source
  code, and they are not part of the release download. On the
  [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser), click **Code**, then
  **Download ZIP**, and extract it anywhere. This guide calls that folder the **source folder**.
- An audio editor that exports WAV, such as Audacity.

## Step 1: Dump the game's audio

1. Press **F1** and open **Workshop > Tools > Asset Dump**.
2. Tick **Audio Samples (WAV)**.
3. Click **Dump selected**, then **Open dump folder**.
4. Keep the complete audio dump, including font metadata. You can leave it beside the game;
   copying it into the source folder is not required.

Run the commands below in **PowerShell, from the source folder**, in the same session. Replace
this example path with your game's actual `dump/audio/` directory:

```powershell
$DumpAudio = "C:/Games/G-Diffuser/dump/audio"
```

Every native names/check/build command uses that same path. Missing playback-rate metadata
blocks packing. Keep the exact dumped key, including double underscores and the original bank
offset/size; the size is not your replacement WAV's size. See
[Music banks and sample keys](CUSTOM_MUSIC.md#read-a-sample-key) for the format and cartridge/EK
distinction. A key appearing in a mixed dump does not prove the active game context uses it.

## Step 2: Find the sound

Open the selected dump's `samples/` directory. Each file is one recording, and its name is its
**key**. These are cartridge-bank examples, not universal EK mappings:

```text
SAMPLE_SOUND_EFFECTS__0x1CD20__4518.wav
SAMPLE_BGM__0x9D9680__16848.wav
```

Sound effects and announcer lines are the **short** files in both groups, `SAMPLE_SOUND_EFFECTS`
and `SAMPLE_BGM`. The long `SAMPLE_BGM` files are songs. Play the short files in any audio player
until you find the one you want, and write down its key exactly.

To list every valid key with its playback rate, run this from the source folder:

```powershell
python tools/gen_sample_pack.py --names --dump-dir "$DumpAudio"
```

## Step 3: Export your recording

1. Open your recording in the audio editor and trim it.
2. Export it as **WAV, 16-bit PCM**. Mono or stereo both work; stereo is mixed down to mono.
3. Make a new, empty folder, for example `my-sounds/`, and save the WAV in it with the **exact key
   as its name**. One folder can hold any number of replacements:

```text
my-sounds/
  SAMPLE_SOUND_EFFECTS__0x1CD20__4518.wav
  SAMPLE_BGM__0x9D9680__16848.wav
```

Any sample rate works. The packer converts each file to the rate the game uses for that key.
Put the WAVs directly in the folder: the packer does not read subfolders.

Your recording does not need to match the stock length.

## Step 4: Build the pack

From the source folder, run:

```powershell
python tools/gen_sample_pack.py my-sounds my-sounds.o2r --name "My Sounds" --author "Your Name" --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py my-sounds.o2r --check --dump-dir "$DumpAudio"
```

Replace the quoted author value with your name. The second command validates the finished pack
and ends with `OK` on success. Read the accepted/skipped/failed counts: if all WAVs are skipped,
the build fails and creates no archive. Do not install stale output from an earlier build.

## Step 5: Install it

1. Copy `my-sounds.o2r` into the `mods/` folder next to the game executable. **Workshop > Mods >
   Open mods folder** opens it for you.
2. On **Workshop > Mods**, tick **Enable sample packs**. It is off by default.
3. Make sure your pack's checkbox is ticked in the pack list.
4. Click **Reload packs**.

Your sounds play the next time the game loads them, usually when the next race or menu starts.

## Sounds that repeat

Without a loop file, every replacement plays once and stops. That is right for most effects and for
every voice line. If the stock sound repeats while it plays, such as an engine hum, add a
`<key>.loop.json` file next to the WAV. [Custom music, step 4](CUSTOM_MUSIC.md#step-4-make-it-loop)
shows how to measure and write it.

## How it behaves

- **One key, every use.** A replacement changes the recording everywhere the game uses it. If a
  sound is shared by several events, all of them change.
- **Shared budget.** All sample replacements, across every installed sample pack, share 1 MiB of
  memory. Short effects use very little: one second at 16000 Hz is about 9 KiB. Long songs in
  another pack can use up the budget, and once it is full, later replacements are refused.
- **Load order.** If two packs replace the same key, the pack lower in the list on
  **Workshop > Mods** wins.

## If something goes wrong

**`--check` says my pack has raw `.wav` files.**
The pack was made by zipping WAVs instead of with the packer, so the game ignores it. Extract
only an archive you trust into a new empty folder, then convert and check it using the same dump:

```powershell
python -m zipfile -e old-pack.o2r old-pack-wavs
python tools/gen_sample_pack.py old-pack-wavs old-pack-samples.o2r --name "My Sounds" --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py old-pack-samples.o2r --check --dump-dir "$DumpAudio"
```

If the WAVs were inside a subfolder of the old pack, move them up to the top of `old-pack-wavs`
before the second command. Then put `old-pack-samples.o2r` in `mods/` in place of the old pack.

**A filename is skipped.**
Copy the entire key from `--names`; do not replace its stock size with your WAV size. A unique
known match may produce a suggestion, but the packer never renames the WAV or its `.loop.json`.
Known samples left as stock are informational; missing rate metadata is a blocking error.

**Nothing changed.**
Check that **Enable sample packs** is ticked, that your pack is ticked, and that each WAV name is
an exact key from step 2. Then start a new race.

**The sound plays too fast or too slow.**
Rebuild with the matching dump selected by `--dump-dir` (step 1). Older packers guessed
the playback rate when metadata was missing; current packers refuse that case.

**Some replacements work and others do not.**
The 1 MiB budget is full. Shorten your recordings or remove other sample packs.

**I need more detail.**
Turn on the log file and search it for `[sample-pack]`. The log names packs that hold raw WAVs,
and it says when sample packs are installed but **Enable sample packs** is off. See
[Logs](../MODDING_GUIDE.md#logs).

## Quick reference

```powershell
# Dump:    F1 > Workshop > Tools > Asset Dump > Audio Samples (WAV) > Dump selected
$DumpAudio = "C:/Games/G-Diffuser/dump/audio"
# Find:    the selected dump's samples/; use the complete key for the intended game context
python tools/gen_sample_pack.py --names --dump-dir "$DumpAudio"

# Name each WAV after its key, all in one folder, then build and validate
python tools/gen_sample_pack.py my-sounds my-sounds.o2r --name "My Sounds" --author "Your Name" --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py my-sounds.o2r --check --dump-dir "$DumpAudio"

# Install: copy my-sounds.o2r into mods/
# Enable:  F1 > Workshop > Mods > Enable sample packs > tick the pack > Reload packs
```
