# Sharing tracks and machines

> Share the tracks you build in Course Edit and the machines you build in Create Machine.

Tracks and machines you make in the Expansion Kit editors are saved on the emulated 64DD disk.
The **Content Library** turns any of them into a single file you can send to another player, and
installs the files other players send you. A track becomes a `.gdxp` package that also carries its
custom music. A machine becomes a `.gdxc` file.

No tools are needed. Everything happens in the game, on **Workshop > Content Library**.

## Share a track or machine

1. Press **F1** and open **Workshop > Content Library**. The table lists every track and machine
   on your disk. If something you just saved is missing, click **Refresh**.
2. Click **Export** on its row. The game writes `exports/<name>.gdxp` (a track) or
   `exports/<name>.gdxc` (a machine) next to the game executable.
3. Send that file to the other player.

Track exports leave out your author ghosts and time records. To include them, tick **Include
author ghosts and time records in track exports** before you export.

### Share your Edit Cup

**Export Edit Cup as bundle** packs your Edit Cup, all of its tracks and their custom music into one
file, `exports/edit-cup.gdxp`.

To add a track to the Edit Cup, click **Edit Cup** on its row, then **Register** on a cup slot.
Replacing a track that is already in a slot asks you to confirm. A cup slot takes the full track
name, up to 20 characters.

A cup with a track name over 8 characters makes a bundle that older builds of the game refuse to
import. A cup of shorter names imports everywhere.

## Install a file someone sent you

1. Copy the `.gdxp` or `.gdxc` file into the `exports/` folder next to the game executable.
2. Open **Workshop > Content Library** and click **Refresh**.
3. Find the file under **Import from exports/** and click **Import**. If it would replace
   something on your disk, the button asks you to confirm first.
4. Open the game's own file browser in Course Edit or Create Machine again to see the new entry.

Import is not available while a race is running.

Before installing, the game checks the file: its checksums, its name, the disk's 100-file limit,
and whether it would overwrite something. A file that fails a check is listed as **Rejected** with
the reason.

## Custom music in a track

A track that uses a custom song from `music/custom/` carries that song inside its `.gdxp`. Importing
the track copies the song into your `music/custom/` folder, so the track plays it right away.

- **Same name, different song.** If you already have a song with the same file name but different
  contents, yours is kept. The imported song is saved under its own name with a code added, such as
  `ocean_theme_1a2b3c4d.ogg`, and the track uses that one.
- **Same song.** If you already have the exact same song, nothing is copied.
- **Size limit.** A song travels only when it is 32 MB or smaller. A larger song is left out, and the
  track plays its stock fallback.

A `.gdxp` is a plain zip file. Older builds of the game do not read it: unzip it into `exports/` and
import the `.gdxc` inside, which works as before.

## Import from an emulator or a 64DD (experimental)

Tracks made in an emulator or on a real 64DD can be read straight from the disk image, with no
conversion.

1. Under **Import from a disk image**, click **Browse...** and pick the image, then **Open
   image**.
2. Click **Import** on each track or machine you want.

Accepted images: `.ndd`, `.ndr`, `.d6r` and `.n64dd` (Project64, Mupen64Plus, RMG, ares, 64DD Dump
Tool), `.disk` and `.disk_save` (ares, MAME, RetroArch), `.d64`, and raw `.ram` dumps (MiSTer). The
game recognizes an image by its contents, so a renamed file still opens. The image is only read,
never changed.

Copy-protected entries are listed as **Refused** and cannot be imported.

## Import courses from a ROM hack (experimental)

Courses in an F-Zero X ROM use the same format as Course Edit tracks. A ROM hack that changes the
course table, such as FZEP tracks, can be read straight into your Content Library.

1. Patch your own clean ROM with the hack, using any patcher.
2. Under **Import courses from a ROM**, click **Browse...** and pick the patched ROM, then **Read
   ROM**.
3. Tick **Only courses this ROM changed** to hide the courses the hack left stock.
4. Click **Import** on each course you want.

Only the courses come across. Anything else the hack changes, such as music or textures, does
not. Both ROMs are only read, never changed.

## Quick reference

```text
Export:   F1 > Workshop > Content Library > Export          -> exports/<name>.gdxp (track)
                                                              -> exports/<name>.gdxc (machine)
Cup:      F1 > Workshop > Content Library > Export Edit Cup as bundle -> exports/edit-cup.gdxp
Install:  copy the .gdxp or .gdxc into exports/ > Refresh > Import from exports/ > Import
Emulator: Import from a disk image > Browse... > Open image > Import
ROM hack: Import courses from a ROM > Browse... > Read ROM > Import
```
