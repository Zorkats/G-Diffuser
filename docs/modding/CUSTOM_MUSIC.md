# Custom music

> Replace the intended song, without confusing cartridge music with Expansion Kit music.

## Replace a complete song (no Python required)

The [G-Diffuser Companion](../COMPANION.md) does these steps for you on Windows: it names the file,
sets the loop on a waveform or suggests one, lets you hear the loop seam, matches the song's level
to the game's music, and keeps the file it replaces. The steps below do the same by hand.

1. Put an Ogg Vorbis (`.ogg`), FLAC (`.flac`), or WAV (`.wav`) recording in `music/` next to
   the executable you actually launch, not another installed copy. Startup creates this folder
   on first and later launches. Use it for both game replacements and custom course songs.
   MP3 is not supported.
2. Rename only the recording's stem for the screen: `bgm_title` for the title screen or
   `bgm_select` for selection menus. Keep its real format extension: a FLAC becomes
   `bgm_select.flac`, not `bgm_select.ogg`. Enable filename extensions in your file manager so
   you can check the complete name. Title and selection are different slots. `bgm_course_editor`
   and `bgm_machine_editor` replace the editor soundtracks. [Music slots](#music-slots) lists
   every stem.
3. Leave and re-enter the screen so its music starts again. **Settings > Audio > Streamed music
   replacement** is on by default; turn it back on only if you switched it off. Music and master
   volume still apply.

For example, a FLAC selection-menu replacement belongs here:

```text
G-Diffuser/
  G-Diffuser.exe
  music/
    bgm_select.flac
    My Racing Song.ogg   (selectable in Course Edit)
```

`music/` takes priority over the legacy `mods/music/` replacement folder, even when the legacy
file uses a higher-priority format. Within each folder, OGG wins over FLAC, then WAV. Names and
extensions are matched without ASCII case distinctions. Keep only the replacement you intend
to hear for each slot. Subfolders are not scanned recursively.
The native sample-pack memory budget below does **not** limit streamed song duration.

### Music slots

Every stem the game recognizes, in game order. The **Streams** column says whether a file in
`music/` replaces that music today. Slots marked **No** start through the sound-effect player
instead of the music player, so a streamed file is ignored and the stock jingle plays.

| Id | Stem | Where it plays | Streams |
|---:|---|---|---|
| 0 | `bgm_mute_city` | Mute City courses | Yes |
| 1 | `bgm_silence` | Silence courses (a song, not a mute slot) | Yes |
| 2 | `bgm_sand_ocean` | Sand Ocean courses | Yes |
| 3 | `bgm_port_town` | Port Town courses | Yes |
| 4 | `bgm_big_blue` | Big Blue courses | Yes |
| 5 | `bgm_devils_forest` | Devil's Forest courses | Yes |
| 6 | `bgm_red_canyon` | Red Canyon courses | Yes |
| 7 | `bgm_sector` | Sector courses | Yes |
| 8 | `bgm_white_land` | White Land courses | Yes |
| 9 | `bgm_rainbow_road` | Rainbow Road | Yes |
| 10 | `bgm_new_03` | No stock course; selectable in Course Edit as "New 03" | Yes |
| 11 | `bgm_new_02` | No stock course; selectable in Course Edit as "New 02" | Yes |
| 12 | `bgm_new_01` | No stock course; selectable in Course Edit as "New 01" | Yes |
| 13 | `bgm_new_04` | No stock course; selectable in Course Edit as "New 04" | Yes |
| 14 | `bgm_title` | Title screen | Yes |
| 15 | `bgm_select` | Mode, machine and course select, and between Grand Prix races | Yes |
| 16 | `bgm_option` | Options and the records course select | Yes |
| 17 | `bgm_deathrace` | Death Race | Yes |
| 18 | `bgm_course_editor` | Course Edit | Yes |
| 19 | `bgm_machine_editor` | Create Machine | Yes |
| 20 | `bgm_ead_demo` | EAD demo | Yes |
| 21 | `bgm_21` | Finish jingle when you lose a race | No |
| 22 | `bgm_22` | Finish jingle, and the winner jingle | No |
| 23 | `bgm_23` | Retire jingle | No |
| 24 | `bgm_24` | Winner jingle when the winner has crashed | No |
| 25 | `bgm_gameover` | Game Over | No |
| 26 | `bgm_end_cs` | Grand Prix ending | No |
| 27 | `bgm_credits` | Staff credits | No |
| 28 | `bgm_demo` | Race intro fanfare | No |
| 29 | `bgm_deathrace2` | Not used: Death Race plays `bgm_deathrace` | No |

Custom tracks use whichever course song their creator picked in Course Edit, or a song from
`music/` chosen in [Track-specific music](#track-specific-music).

### Loop a streamed recording

**Without embedded loop metadata, the whole recording repeats.** To play an introduction once
and repeat a chosen region, embed the loop in the audio file:

| Recording format | Loop information read by the game |
|---|---|
| Ogg Vorbis or FLAC | `LOOPSTART` and `LOOPLENGTH` comment tags inside the recording |
| WAV | A `smpl` loop inside the WAV |

**Loose `bgm_select.json` and `.loop.json` files are not read for streamed music.** The JSON
procedure in [native sample-pack step 4](#step-4-make-it-loop) belongs to a different workflow.
`LOOPEND` alone is not a substitute for `LOOPLENGTH`.

For Ogg/FLAC, use the recording's **source sample frames**, not milliseconds, bytes, or the
combined number of left/right samples. A stereo frame contains both channels. In an audio
editor that can write Vorbis comments, add these custom tags before exporting the recording:

```text
LOOPSTART  = first frame to repeat
LOOPLENGTH = end frame - first frame
```

The end frame is exclusive. Both tags must be whole numbers, the length must be positive, and
the region must fit inside the recording. For times measured in seconds, multiply each time by
the exported sample rate and round to a whole frame. At **44100 Hz**, a region from **2 to 10
seconds** uses `LOOPSTART=88200` and `LOOPLENGTH=352800`. Use your own rate and region; these
example numbers are not universal loop points.

If you have **FFmpeg installed** and `ffmpeg`/`ffprobe` available in your terminal, this PowerShell
example writes those tags into a separate FLAC without re-encoding its audio. Run it in the
folder containing `bgm_select.flac`; the recording must be at least ten seconds long. Run the
first command separately: if it does not report `44100`, recalculate the loop values before
running the second command.

```powershell
ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of default=noprint_wrappers=1:nokey=1 "bgm_select.flac"
ffmpeg -n -i "bgm_select.flac" -map 0:a:0 -c:a copy -metadata LOOPSTART=88200 -metadata LOOPLENGTH=352800 "bgm_select-looped.flac"
ffprobe -v error -show_entries format_tags=LOOPSTART,LOOPLENGTH -of json "bgm_select-looped.flac"
```

For this example, the last command must show `LOOPSTART=88200` and `LOOPLENGTH=352800`.
Substitute your calculated values in the second command. `-n` refuses to overwrite an existing
output; choose another output name if it already exists. Close the game, keep a backup of the
original outside the searched music folders, and install the tagged output as
`music/bgm_select.flac`. The temporary name `bgm_select-looped.flac` is not a recognized menu slot.
Restart and listen through the loop boundary. Decoder/tag checks do not prove that the selected
cut is seamless.

If you already have JSON loop points, first establish which recording and sample rate they
describe. Native-pack points use the **converted** recording's frames. Do not copy those numbers
unchanged into a stream with a different sample rate; recalculate from the intended times.

Course music starts from the beginning at every race start, including a retry, as the stock music
does. To hear a loop that sits late in a course song, play a race longer than the loop end, or use
**Hear the loop seam** in the Companion.

### Match the game's music level

**A recording without a level tag plays as it was mastered.** Modern masters are often about
12 dB louder than the game's own music and drown out the sound effects. The game reads a level tag
and plays the recording at its stock music level, about -17 LUFS:

| Recording format | Level information read by the game |
|---|---|
| Ogg Vorbis or FLAC | `REPLAYGAIN_TRACK_GAIN` (for example `-12.34 dB`) and `REPLAYGAIN_TRACK_PEAK` (for example `0.987654`) comment tags |
| WAV | The loudness and true peak fields of a version 2 `bext` chunk (EBU Tech 3285) |

The track gain follows ReplayGain 2, which brings a recording to -18 LUFS; the game adds 1 dB to
reach its own level. A gain written by another ReplayGain tool works too, as long as it uses the
same reference. The game never raises a recording so far that its peak passes full scale, and
without a peak tag it only turns a recording down. Album gain tags are ignored.

The Companion measures the recording and writes these tags when it installs it, with **Match the
game's music level** on, without re-encoding the audio. **Settings > Audio > Match custom music
loudness** turns the game's use of the tags off; it is on by default and applies from the next
song. The game log names the gain it applies, for example
`[music] bgm_title: level -12.03 dB (track gain -13.03 dB, peak 0.977)`.

### Track-specific music

For a **track-specific soundtrack**, put a supported recording directly in `music/` with a
descriptive name, such as `My Racing Song.ogg`. Restart the game, then select the song with the
Course Edit music controls. Recognized game-slot names such as `bgm_select` are reserved for
global replacements and do not appear as course songs from this folder. Rename a song if you
want to select it only for a course. Explicit preview does not depend on native BGM being active.
Pausing ducks preview to 40%; stopping cuts it immediately. Normal race/menu replacements still
follow the native fade-out.
[Track packages carry their custom music](SHARING_TRACKS_AND_MACHINES.md#custom-music-in-a-track).

**Existing installations:** `mods/music/custom/` and `music/custom/` remain course-library
fallbacks, in that order after `music/`. The game does not create these legacy folders or move
or delete their files. Existing course choices keep their keys; a matching song in `music/`
wins after restart. For equal-priority duplicate names, filename order breaks ties consistently.
Newly imported songs go into `music/`. A legacy course song with a reserved game-slot name is
reused from its old folder when identical, or imported with a content code added so it cannot
replace menu music. You do not need to create another music folder.

## Advanced: native sample packs

Use this path to replace a native recording, not to stream a complete song. Only this path needs
Python, bank keys, and dumped playback-rate metadata. A valid `.o2r` does not prove the active
screen uses its sample. A matching streamed replacement takes precedence over native music.

### Choose the game context and screen first

Choose **cartridge F-Zero X** or **64DD Expansion Kit**, then the screen you want to change.
This means the music context playing in the game, not the executable's name or whether EK files
exist on your computer. A dump may list both contexts; a listed key is not active-mode detection.

These mappings are verified for **US rev0**:

| Music context | Screen | Streamed stem (no bank choice) | Native sample key |
|---|---|---|---|
| Cartridge | Title | `bgm_title` | `SAMPLE_BGM__0x437740__473374` |
| Cartridge | Selection | `bgm_select` | `SAMPLE_BGM__0x4AB060__249084` |
| Expansion Kit | Title | `bgm_title` | `SAMPLE_DDBGM_TITLE__0x0__473374` |
| Expansion Kit | Selection | `bgm_select` | `SAMPLE_DDBGM_SELECT__0x0__249084` |

All four native menu resources are mono at 22050 Hz. `SAMPLE_BGM` names the cartridge bank;
`SAMPLE_DDBGM_*` names EK music banks. **Do not change a bank prefix by hand.** Copy the complete
key for the intended context and screen. A cartridge selection key cannot replace EK title music.

The versioned catalog is `tools/music_targets.json`. It covers title, selection, and EK Big Blue,
not every song. The `--targets` command below prints it. For another target, use matching dumped
metadata and identify the native resources; do not guess a mapping or playback rate.

### Read a sample key

The exact format is `<BANK>__0x<ADDR>__<SIZE>`, with **two underscores** at each separator:

- `BANK` identifies the original bank.
- `ADDR` is the original sample's hexadecimal offset within that bank.
- `SIZE` is the original sample's size, not your replacement WAV's size or duration.

Treat the whole key as an identity. Keep its numbers unchanged when exporting a longer recording.
The WAV and optional `.loop.json` must have the same complete basename.

| Filename | Result |
|---|---|
| `SAMPLE_DDBGM_SELECT_0x0_249084.wav` | Wrong: single underscores; skipped. |
| `SAMPLE_DDBGM_SELECT__0x0__249084.wav` | Canonical EK selection filename. |

> **Native budget:** all installed sample replacements share **1 MiB**, including effects and
> voices. Roughly 80 seconds of one 22050-Hz mono recording can fill it. Two simultaneous
> 32000-Hz resources fill it in roughly 29 seconds. That is not a universal EK duration limit:
> EK title and selection each use one 22050-Hz resource. Prefer streaming for long recordings.

## What you need

- G-Diffuser, already set up and running.
- **Python 3.8 or later** for the native packer; no extra Python packages are needed.
- The **source folder** containing `tools/`. Pack scripts are not in the release download.
  On the [G-Diffuser GitHub page](https://github.com/Zorkats/G-Diffuser), select **Code > Download
  ZIP** and extract it, or use your source checkout.
- An audio editor that exports WAV, such as Audacity.

Run the following commands in **PowerShell, from the source folder**, in the same session.
Replace the example dump path with your game's actual `dump/audio/` path. Input folders such as
`my-music/` are relative to the source folder; the dump can stay elsewhere.

## Step 1: Dump the game's audio

1. Press **F1** and open **Workshop > Tools > Asset Dump**.
2. Tick **Audio Samples (WAV)**.
3. Click **Dump selected**, then **Open dump folder**. Keep the complete audio dump, including
   its font metadata, not only `samples/`.
4. Set the path and inspect the catalog:

```powershell
$DumpAudio = "C:/Games/G-Diffuser/dump/audio"
python tools/gen_sample_pack.py --targets
python tools/gen_sample_pack.py --names --dump-dir "$DumpAudio"
```

Use the **same `$DumpAudio`** for every names, check, and build command below. Missing stock WAVs
are informational: EK keys and rates can be listed even when their disk audio was not written as
WAV. Missing playback-rate metadata is different: the packer refuses it rather than guessing.

## Step 2: Find the song

For the menu targets, choose a row in the table above. The rest of this worked example replaces
**Expansion Kit selection**, not title or cartridge selection:
`SAMPLE_DDBGM_SELECT__0x0__249084`.

For other dumped recordings, listen to files in the selected dump's `samples/` directory and
copy the matching key exactly. Large `SAMPLE_BGM__*.wav` files are cartridge music recordings;
an EK resource may have metadata but no stock WAV. Do not substitute a cartridge key for it.

### Songs with more than one native resource

For a streamed Big Blue replacement, use the stem `bgm_big_blue`. Native EK Big Blue instead
uses both of these 32000-Hz resources:

| Native resource identity | Exact key |
|---|---|
| `part_0` | `SAMPLE_DDBGM_BIG_BLUE__0x0__1095310` |
| `part_1` | `SAMPLE_DDBGM_BIG_BLUE__0x10B690__1095310` |

Supply one mono WAV per resource. The labels identify native parts, not a verified left/right
mapping. The packer downmixes stereo WAVs; it does not split or duplicate them automatically.
You may deliberately provide the same mono recording under both keys, but this does not preserve
stereo. Both resources consume the shared budget. Other songs need their own verified mapping.

## Step 3: Export your recording

1. Trim your recording with the shared budget in mind. The EK selection example uses one
   22050-Hz resource, not the two-resource Big Blue budget.
2. Export **WAV, 16-bit PCM**. Mono or stereo input works; stereo is mixed down to mono.
3. Create an empty `my-music/` folder in the source folder. Save the WAV with the chosen key:

```text
my-music/
  SAMPLE_DDBGM_SELECT__0x0__249084.wav
```

The input sample rate may differ. The packer converts it to the stock rate from `$DumpAudio`.
Put WAVs directly in this folder, not in subfolders. Your recording need not match the stock length.

## Step 4: Make it loop

**Native sample packs only.** For a loose song in `music/`, follow
[Loop a streamed recording](#loop-a-streamed-recording) instead; do not create a JSON sidecar.

Without a loop sidecar, a native replacement plays once. Measure the converted recording first:

```powershell
python tools/gen_sample_pack.py my-music --check --dump-dir "$DumpAudio"
```

For an 80-second, 44100-Hz input, a line can look like this:

```text
SAMPLE_DDBGM_SELECT__0x0__249084: 3528000 samples @ 44100 Hz -> 1764000 decoded samples, one-shot
```

Use **your own command's decoded count**, not the example number. Create the matching sidecar:

```text
my-music/
  SAMPLE_DDBGM_SELECT__0x0__249084.wav
  SAMPLE_DDBGM_SELECT__0x0__249084.loop.json
```

For that example count:

```json
{"start": 0, "end": 1764000}
```

`start` must be a multiple of 16, and `0 <= start < end <= decoded count`. With `start: 0` and
`end` equal to the decoded count, the whole recording repeats. Re-run the check after editing
and confirm it reports your loop rather than `one-shot`.

## Step 5: Build the pack

From the same PowerShell session and source folder, run:

```powershell
python tools/gen_sample_pack.py my-music my-music.o2r --name "My Music" --author "Your Name" --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py my-music.o2r --check --dump-dir "$DumpAudio"
```

Replace the quoted author value with your name. Encoding a long recording can take time.
The build reports accepted, skipped, and failed files; the finished archive check ends with `OK`
on success. Read warnings even when the command succeeds. If every WAV is skipped, the command
fails with `nothing to pack` and creates no archive. Do not install an older file as if it were
that failed build's output.

## Step 6: Install it

1. Copy `my-music.o2r` into `mods/` next to the game executable. **Workshop > Mods > Open mods
   folder** opens it.
2. Tick **Enable sample packs** on **Workshop > Mods**, then tick your pack in the list.
3. Click **Reload packs**.
4. Disable the matching streamed replacement while testing native music. Leave and re-enter
   the intended screen so its native resource loads again. For this example, test **EK selection**.

An already playing resource does not become the replacement merely because the archive validates.

## If something goes wrong

**A streamed menu replacement is not found.** Check the actual launched executable's `music/`
folder, not `music/custom/` or another installation. Check the stem and real extension against
[Music slots](#music-slots), check that streamed replacement is still on, and re-enter the
intended screen. Jingles, Game Over, the ending and the credits cannot be streamed yet. With [logging enabled](../MODDING_GUIDE.md#logs),
`[music] BGM bgm_select (id 15): no file, stock track` means no matching stream was found, not a
decoder failure. A found replacement logs its path. Check for higher-priority formats in
`music/`. A legacy `mods/music/` replacement is used only when `music/` has no matching file.

**A streamed song ignores my chosen loop region.** Check the embedded tags or WAV loop, not a
loose JSON sidecar. Follow [Loop a streamed recording](#loop-a-streamed-recording). Without valid
metadata the whole file repeats. If playback stops completely, include the log when reporting
the problem; missing loop tags alone do not explain a failure to repeat the whole file.

For a valid pack that does not change the music, check these in order:

1. **Context and screen:** title is not selection, and cartridge is not EK. Compare the exact key
   with the target table. A successful archive check is not proof of the target used in-game.
2. **Streamed precedence:** disable streamed replacement, or remove conflicting slot files from
   `mods/music/` and `music/` while the game is closed. Keep backups of your recordings.
3. **Enable switches:** enable sample packs and the pack itself on **Workshop > Mods**.
4. **Key and metadata:** copy the complete key and rebuild with the same matching `$DumpAudio`.
   For wrong pitch or speed, check that dump's stock rate, not just your WAV's rate.
5. **Memory:** all installed native sample packs share 1 MiB. Shorten recordings or disable
   other packs if the log reports a refused replacement.
6. **Reload:** click **Reload packs**, then leave and re-enter the screen or start another race.

**The packer skips `SAMPLE_DDBGM_SELECT_0x0_249084.wav`.**
Use `SAMPLE_DDBGM_SELECT__0x0__249084.wav`, with double underscores. A unique known match produces
an exact suggestion, not an automatic rename. Rename its `.loop.json` basename too. Known samples
left as stock need no replacement WAV. `Nothing to pack` is a failure, not a usable output.

**A native sample replacement stops after one play.** Add the matching sidecar from step 4,
check its converted loop bounds, and rebuild. This sidecar does not configure streamed songs.

**I need more detail.** Enable the log and look for `[sample-pack]` rejection reasons.
See [Logs](../MODDING_GUIDE.md#logs). Include the screen, cartridge/EK context, route (stream or
native), exact filename, and log when reporting a problem.

## Advanced: sequence packs

A **sequence** is the N64's note data: which instrument plays which note, and when. Sequence packs
replace that data, not a recorded song. They need N64 sequence tools; the game does not read MIDI.
The packer takes `<name>.seq` files in raw N64 sequence format:

```sh
python tools/gen_sequence_pack.py --names
python tools/gen_sequence_pack.py my-seqs my-seqs.o2r --name "My Sequences" --author "Your Name"
python tools/gen_sequence_pack.py my-seqs.o2r --check
```

Install the pack and tick **Enable sequence packs** on **Workshop > Mods**.

- **Keep each replacement within its stock sequence size.** Larger data is refused; stock plays,
  and the log shows `[seq-pack]`. The packer warns when sizes differ.
- **Instruments come from the slot.** When moving a sequence to another slot, add `<name>.font`
  beside `<name>.seq` with the original slot's name, such as `ddbgm_mute_city`. Otherwise it can
  request the wrong instruments and play silence.
- **Changes apply at the next load**, like sample packs. **Music (MIDI)** in Asset Dump writes
  reference MIDI; it does not supply a MIDI import workflow.

## Quick reference

For complete recordings, start with the no-Python path. For native EK selection, prepare the
canonical WAV and optional loop sidecar from steps 2-4, then run from the source folder:

```powershell
$DumpAudio = "C:/Games/G-Diffuser/dump/audio"
python tools/gen_sample_pack.py --targets
python tools/gen_sample_pack.py --names --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py my-music --check --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py my-music my-music.o2r --name "My Music" --author "Your Name" --dump-dir "$DumpAudio"
python tools/gen_sample_pack.py my-music.o2r --check --dump-dir "$DumpAudio"
```

Copy the successful output into `mods/`, enable sample packs and the pack, reload, then re-enter
EK selection with the matching streamed override disabled. Use a different complete key when
choosing another context or screen.
