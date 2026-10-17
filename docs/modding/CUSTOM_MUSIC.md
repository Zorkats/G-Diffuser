# Custom music

> Replace a song in the game with your own recording.

F-Zero X stores each song as **one long recorded loop**. The game's music "sequences" are tiny
stubs that only start that recording. To change a song, you replace its recording with your own
audio. That means music mods use **sample packs**, the same channel as sound effects.

The loop is: **dump** the game's audio, **find** the song you want to replace, **export** your
recording as a WAV with the same name, **pack** it, and **install** it.

> **Length limit:** all sample replacements share **1 MiB** of memory. At the cartridge music
> rate (22050 Hz) that is **about 80 seconds** of audio in total. Expansion Kit songs need two
> recordings at a higher rate, so they get about 29 seconds. A short loop fits. A full
> four-minute track does not, so cut a loopable section of it.

## What you need

- G-Diffuser, already set up and running.
- **Python 3.8 or later.** The music tool needs nothing else.
- The **pack tools**. They are the Python scripts in the `tools/` folder of the G-Diffuser source
  code, and they are not part of the release download. On the
  [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser), click **Code**, then
  **Download ZIP**, and extract it anywhere. This guide calls that folder the **source folder**.
- An audio editor that exports WAV, such as Audacity.

## Step 1: Dump the game's audio

1. Press **F1** and open **Workshop > Tools > Asset Dump**.
2. Tick **Audio Samples (WAV)**.
3. Click **Dump selected**, then **Open dump folder**.
4. Copy the whole `dump/` folder into the source folder, so that `dump/audio/` sits next to
   `tools/`.

The packer reads `dump/audio/` from the source folder. That is how it knows which names are valid
and which rate each recording plays at. Without it, your audio plays at the wrong speed.

## Step 2: Find the song

Open `dump/audio/samples/`. Every file is one recording. The songs are the large files whose names
start with `SAMPLE_BGM`, for example:

```text
SAMPLE_BGM__0x0__533908.wav
```

Play them in any audio player until you find the song you want to replace. The file name is the
song's **key**. Write it down exactly.

> **Expansion Kit songs** have keys that start with `SAMPLE_DDBGM_`. They come from the 64DD
> disk, so the dump lists their keys but writes no WAV for them. Run
> `python tools/gen_sample_pack.py --names` from the source folder to see every key.
>
> Each Expansion Kit song is **two recordings** of the same length that play together, most
> likely its left and right channels. For example, Big Blue is
> `SAMPLE_DDBGM_BIG_BLUE__0x0__1095310` and `SAMPLE_DDBGM_BIG_BLUE__0x10B690__1095310`. Replace
> both, with the same WAV under each name if your audio is mono. They play at 32000 Hz, so the
> budget allows about 29 seconds per song.

## Step 3: Export your recording

1. Open your recording in the audio editor and cut the part you want: at most about 80 seconds, or
   29 seconds for an Expansion Kit song.
2. Export it as **WAV, 16-bit PCM**. Mono or stereo both work. The packer mixes stereo down to
   mono, because each game recording is a single channel.
3. Make a new, empty folder, for example `my-music/`, and save the WAV in it with the **exact key
   as its name**:

```text
my-music/
  SAMPLE_BGM__0x0__533908.wav
```

Any sample rate works. The packer converts your audio to the rate the game uses for that song.

## Step 4: Make it loop

Without a loop file, your song plays once and then stops. To loop it, first ask the packer how
long the converted song is:

```sh
python tools/gen_sample_pack.py my-music --check
```

The output has a line like this:

```text
SAMPLE_BGM__0x0__533908: 3528000 samples @ 44100 Hz -> 1764000 decoded samples, one-shot
```

Create a file next to the WAV named after the key plus `.loop.json`, and use that last number as
the loop end:

```text
my-music/
  SAMPLE_BGM__0x0__533908.wav
  SAMPLE_BGM__0x0__533908.loop.json
```

```json
{"start": 0, "end": 1764000}
```

`start` is where each repeat begins. It must be a multiple of 16. `0` loops the whole recording.

## Step 5: Build the pack

From the source folder, run:

```sh
python tools/gen_sample_pack.py my-music my-music.o2r --name "My Music" --author <your-name>
```

Encoding a long song takes a while. The packer checks its own result and warns if the quality is
low. To validate a finished pack, run:

```sh
python tools/gen_sample_pack.py my-music.o2r --check
```

## Step 6: Install it

1. Copy `my-music.o2r` into the `mods/` folder next to the game executable. **Workshop > Mods >
   Open mods folder** opens it for you.
2. On **Workshop > Mods**, tick **Enable sample packs**. It is off by default.
3. Make sure your pack's checkbox is ticked in the pack list.
4. Click **Reload packs**.

The new song plays the next time the game loads it, for example when the next race starts. A song
that is already playing keeps playing the stock audio until then.

## If something goes wrong

**The song did not change.**
Check that **Enable sample packs** is ticked, that your pack is ticked, and that the WAV name is
the exact key from step 2. Then start a new race.

**The song plays too fast or too slow.**
The `dump/` folder was missing from the source folder when you built the pack. Copy it in
(step 1) and rebuild.

**Some replacements work and others do not.**
All sample replacements share the 1 MiB budget, across every installed sample pack. Once it is
full, later replacements are refused. Shorten your audio or install fewer packs.

**The song stops after playing once.**
Add the `.loop.json` file from step 4 and rebuild.

**I need more detail.**
Turn on the log file and search it for `[sample-pack]`. Each refused replacement is logged with
its reason. See [Logs](../MODDING_GUIDE.md#logs).

## Advanced: sequence packs

A **sequence** is the N64's note data: which instrument plays which note, and when. Sequence packs
replace that data instead of a recording. They suit chiptune-style or note-based music, and need
N64 sequence tools. They cannot carry a recorded song.

The packer takes a folder of `<name>.seq` files in the raw N64 sequence format:

```sh
python tools/gen_sequence_pack.py --names
python tools/gen_sequence_pack.py my-seqs my-seqs.o2r --name "My Sequences" --author <your-name>
python tools/gen_sequence_pack.py my-seqs.o2r --check
```

Install the pack like any other, and tick **Enable sequence packs** on **Workshop > Mods**.

Rules:

- **Your sequence must not be larger than the stock one.** Each sequence loads into memory sized
  for the original. A larger replacement is refused, the stock sequence plays, and the log shows
  a `[seq-pack]` line. The packer warns when the size differs from the stock size.
- **Instruments come from the slot.** Each slot uses its own instrument set. If you move a
  sequence from one slot to another, add a text file named `<name>.font` next to `<name>.seq`
  that contains the original slot's name, for example `ddbgm_mute_city`. Without it, the notes ask
  the wrong instrument set and usually play silence.
- **Changes apply at the next load,** like sample packs.

The **Music (MIDI)** class in **Asset Dump** writes MIDI versions of the stock sequences for
reference. The game does not read MIDI files.

## Quick reference

```sh
# Dump:    F1 > Workshop > Tools > Asset Dump > Audio Samples (WAV) > Dump selected
# Setup:   copy the game's dump/ folder into the source folder
# Find:    dump/audio/samples/SAMPLE_BGM__*.wav, one song per file

# Measure, then write <key>.loop.json with {"start": 0, "end": <decoded samples>}
python tools/gen_sample_pack.py my-music --check

# Build and validate
python tools/gen_sample_pack.py my-music my-music.o2r --name "My Music" --author <your-name>
python tools/gen_sample_pack.py my-music.o2r --check

# Install: copy my-music.o2r into mods/
# Enable:  F1 > Workshop > Mods > Enable sample packs > tick the pack > Reload packs
```
