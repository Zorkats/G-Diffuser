# Course elements

> How Course Edit names venues, skyboxes, music and track effects, how a pack adds names through a manifest, and how a course keeps a name that its stock numbers cannot hold.

A course file stores its venue, skybox and music as plain numbers. Those numbers only cover the stock choices, so a new element cannot be written into the course file. Course Edit therefore gives every element a text **key**, such as `venue.sector`, and the course always keeps a valid stock number as its **fallback**. A build that does not know a key plays the fallback instead.

## The five kinds

| Kind | Stock entries | Key prefix | Manifest kind |
| --- | --- | --- | --- |
| Venue | 11 (IDs 0 to 10) | `venue.` | `venue` |
| Skybox | 8 (IDs 0 to 7) | `skybox.` | `skybox` |
| Music | 14 (raw IDs 0 to 13) | `bgm.` | `bgm` |
| Part style | 9 (IDs 0 to 8) | `part.` | `part` |
| Track effect | 8 (IDs 0 to 7) | `effect.` | `effect` |

Music keys use the raw game IDs. The menu lists the songs in a different order, so `bgm.new_04` always means raw ID 13, whatever row the menu shows it on.

## Keys

- 1 to 48 characters.
- Only lowercase letters, digits, dots, underscores and dashes.
- A stock key names one stock entry and never changes. An extension key is chosen by whoever registers it, and it must not repeat a key of the same kind.
- A kind holds up to 32 registered extension keys in one session. A course holds up to 128 records.

Stock keys:

```text
venue:   venue.mute_city venue.port_town venue.big_blue venue.sand_ocean venue.devils_forest
         venue.white_land venue.sector venue.red_canyon venue.fire_field venue.silence venue.ending
skybox:  skybox.purple skybox.turquoise skybox.desert skybox.blue skybox.night
         skybox.orange skybox.sunset skybox.sky_blue
music:   bgm.mute_city bgm.silence bgm.sand_ocean bgm.port_town bgm.big_blue bgm.devils_forest
         bgm.red_canyon bgm.sector bgm.white_land bgm.rainbow_road bgm.new_03 bgm.new_02
         bgm.new_01 bgm.new_04
part:    part.pit part.dash part.dirt part.slip part.jump part.trap part.gate part.building part.sign
effect:  effect.none effect.pit effect.dirt effect.dash effect.ice effect.lava
         effect.magnet_left effect.magnet_right effect.magnet_middle
```

## Packs: `mods/<pack>/elements.txt`

Any folder under `mods/` that holds an `elements.txt` is a pack. The packs are read at boot, in folder name order. Each line of the manifest adds one element.

```text
kind|key|name|fallback
kind|key|name|fallback|path      (bgm only: a music file)
```

- `kind` is one of `venue`, `skybox`, `bgm`, `part`, `effect`.
- `key` follows the rules in [Keys](#keys) and must match the kind's prefix.
- `name` is the display name, 1 to 64 printable ASCII characters.
- `fallback` is the decimal stock ID the course stores when the key is unknown. It must be a stock ID of that kind (for a skybox, 0 to 7).
- `path` is only for `bgm`. It is relative to the pack folder, has no `..`, no leading `/` or `\`, no `:`, and ends in `.ogg`, `.flac` or `.wav`. The file must exist.

Rules for the file:

- Lines are at most 256 bytes. A longer line is rejected.
- Blank lines and lines that start with `#` are skipped. CRLF line endings are accepted.
- Exactly 4 fields, or 5 for `bgm`. More or fewer is rejected.
- Only printable ASCII is allowed. A control byte rejects the line.
- A rejected line is logged with its number and skipped, and the rest of the file still loads:

  ```text
  [elements] <path>/elements.txt: line 7: fallback must be a decimal stock id, skipped
  ```

- A key that is already registered is logged as a duplicate and skipped. The first registration wins.

Example, `mods/my_pack/elements.txt`:

```text
# one element per line
bgm|bgm.custom.my_theme|My Theme|13|music/my_theme.ogg
skybox|skybox.my_sunset|My Sunset|6
venue|venue.my_city|My City|0
```

`venue`, `part` and `effect` entries register, and they appear in the log as "(no menu in this build)". Only the `bgm` and `skybox` kinds have menu rows. For `bgm`, the file is registered as a music choice. See [Custom music](CUSTOM_MUSIC.md) for the file rules.

## Background rows (skyboxes)

The Course Edit Background menu lists the 8 stock skyboxes first. Registered skybox entries follow them as extra rows, in the same way that the 7A BGM menu lists custom songs after the 14 stock entries.

- Selecting a stock row clears any custom choice of the track.
- Selecting a custom row stores its key in the track's record and sets the track's skybox byte to the entry's fallback, a stock skybox from 0 to 7. The course data never holds a key.
- When the track is loaded for a test run or from a disk Edit Cup slot, `gdx_racer_resolve_course_skybox` (`decomp/src/game/racer.c`) asks the store for the record's fallback and applies it in `Course_Init` (`decomp/src/game/course.c`), before the skybox is set up. Other courses keep their stored byte.
- A key that is not registered in the current session resolves to nothing, so the stored fallback plays.

**Custom skybox imagery is out of scope.** A custom skybox row shows a name and plays one of the 8 stock skyboxes. No pack can add a new sky picture, a new sky model or a sky texture in this build.

## Add an element in code

Code can register an element directly. A pack is the same registration, from a manifest.

1. **Pick a key and a fallback.** The key follows the rules above. The fallback is the stock entry closest to your element: it is what players without your build get.
2. **Register it at startup**, once per session, in `port/` code that runs before Course Edit opens:

   ```c
   #include "gdx_course_registry.h"

   int rc = gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "venue.my_city", "My City",
                                         0 /* fallback: Mute City */);
   if (rc != GDX_COURSE_OK) {
       /* key taken, invalid key or name, bad fallback, or the kind is full */
   }
   ```

3. **Tag the course that uses it.** The course id is its file name and extension, such as
   `MYTRACK.CRSD`. Use `GDX_COURSE_SCOPE_COURSE` for a course-wide element, or a control point
   index from 0 to 63 for a per-point one:

   ```c
   gdx_course_ext_set("MYTRACK.CRSD", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE, "venue.my_city");
   ```

   The course data keeps the fallback ID. Never write a new number into it.
4. **Make the element do something.** The registry only names elements. Your code decides what a
   key changes at runtime, by asking `gdx_course_registry_resolve` or `gdx_course_ext_records`
   where the game loads that kind of element:

   | Kind | Where the game loads it |
   | --- | --- |
   | Venue | `func_800747EC` and `func_80077AD8`, called on course load in `decomp/src/overlays/course_edit/19C470.c` |
   | Skybox | `gdx_course_skybox_resolve`, applied in `Course_Init` (`decomp/src/game/course.c`) for test runs and disk Edit Cup slots |
   | Building art | `gdx_load_venue_building_texture` in `port/n64_gfx_bridge.cpp` |
   | Music | `Audio_DDBgmStart2` in `decomp/src/audio/disk/external.c` |

5. **Export and check.** Export the course from the Content Library. The `.gdxp` it writes should
   hold a `.gdxe` beside the `.gdxc` (open it as a zip). Run `gdx_course_registry_tests` after any
   change to the registry code.

## Saved keys and titles

The keys of each course and its title are kept in one file beside the game executable:

```text
course-elements.gdxs
```

- **Format (GXS1).** A 12 byte header: magic `GXS1`, version 1 (u16), entry count (u16), and the file length (u32). Then one entry per course: the course id (`NAME.EXT`), a bind CRC of that id, a flags byte (bit 0 for keys, bit 1 for a title), the key block (`GXE1`, the sidecar format below) when present, and the title block (`GXT1`) when present. The file ends with a CRC-32 of every byte before it.
- **Unknown keys survive.** The reader checks the structure and the CRC but not the registry. A key from a pack that is not installed stays in the file and comes back when that pack returns. Loading and saving the file does not change its bytes.
- **Load.** The file is read once, the first time a Course Edit screen, a race start or an import needs it. A missing file is an empty store. A file that fails any check is renamed to `course-elements.gdxs.bad` and the store starts empty. A damaged file is never overwritten.
- **Save.** A change writes `course-elements.gdxs.tmp` and renames it over the file. Nothing is saved before a successful load, so a read error cannot erase the file.
- **Limits.** The store holds 128 courses with keys and 128 titles. A new course past the limit is refused with an error; nothing is dropped.

### Renames and deletes

Successful native rename and delete operations (disk ops 15 and 16 in
`decomp/src/overlays/expansion_kit/AB150.c`) call the state hooks in
`port/gdx_course_state.cpp`. They rename or clear the course's keys, title, forks, and settings,
then flush the stores. The hooks do not provide a recoverable multi-file transaction or report a
sidecar flush failure to the native prompt. They are not a Content Library delete/restore feature.

### Content Library deletion and recovery

Back up your save set before using deletion. For a first check, use a disposable track, not a valued
course. The Content Library transaction is separate from the native game's delete prompt.

1. Leave gameplay and select an installed track in the Content Library. Locked `CRSE` tracks can
   be selected for deletion, but cannot be assigned to Edit Cup slots through the library.
2. Open **Track deletion and recovery** and choose **Prepare deletion of selected track**.
3. Check the exact file name and affected cup-slot count. Type the track name exactly, then choose
   **Confirm deletion**. Preparation does not delete anything. **Cancel deletion** closes the preview.
4. Wait for **Track deleted. Its recovery record is retained for Restore.** The library and cup
   selection refresh after completion. Shared music files remain installed.

Deletion removes the track file, its referencing Edit Cup slots, and its per-track extensions,
title, forks, settings, and music binding. The recovery record retains the complete native track
file, including its embedded records and ghosts, plus the affected metadata and cup slots.

The action rejects gameplay, disk/import/cup work, changed selections, unavailable metadata, and
the last edited track while its editor identity remains active. Leaving Course Edit alone does not
clear that identity. Restart the application without reopening that track if this guard applies.
Cancellation is available only before transaction persistence starts. Do not close the application
while it reports **Persisting transaction**.

**Restart finishes confirmed operations.** An interrupted deletion resumes deletion, not automatic
restoration. An interrupted Restore resumes restoration. Writes remain blocked until recovery
completes or the conflict is resolved. Use **Check / retry recovery** after resolving a reported I/O
error or conflict. Corrupt, unsupported, wrong-disk, and wrong-directory records fail closed.

To restore a deleted track, find its **deleted** record, choose **Restore...**, and confirm. Restore
requires the original track name, per-track metadata, and affected cup slots to remain unused. It
does not replace later edits or restore a whole save image. A full metadata store or insufficient
disk space also prevents Restore. The disk-space preflight reserves conservative block-rounding
and temporary-file capacity, so it can refuse a restore that would fit with tighter packing.

Recovery files are stored beside the writable disk-save sidecar, in `<save>.gdd.recovery/`, as
bounded, checksummed `.gtr` records. The panel displays the resolved directory. Keep this directory
with the matching `.gdd` and `course-elements.gdxs`, `course-forks.gbs`, and `course-settings.gks`
when backing up or moving the save set. Do not edit recovery records or use an older build while
an operation is pending. The recovery lock coordinates builds that support it, not older versions.
Completed records remain retained, with a maximum of 64 records and no automatic purge.

Synthetic unit tests cover interruption and conflict handling. Native startup recovery, game UI,
and operating-system power-loss behavior still require runtime acceptance. These tests do not
establish that a real disk, storage device, or operating system survives every interruption.

## Course Edit settings and forks

### Select and edit a path

Open **Point and track settings** in Course Edit. Its path selector is separate from the
collapsible settings section, so you can switch A/B without using Split. Inactive nodes remain
visible in Course and Point modes. In Course mode, hold Ctrl and click an inactive node to switch
paths and select it; an ordinary click keeps the course-placement gesture. In Point mode, click
the inactive node. Switching is disabled during a native grab or viewport drag.

Enable Course Edit mouse input to select inactive nodes with the mouse and use typed tool values.
For Width, Center, or Bank, select the tool and at least one point, then press Enter. Type an
integer and press Enter to apply, or Escape to cancel. Width is the total road width shown in the
editor, `(radiusLeft + radiusRight) / 10`; Center is the lateral offset in the same displayed units.
The editor clamps the radii to its native bounds and rounds them to its 10-unit grid. Bank wraps
and rounds to the nearest 3 degrees. Controls Help shows your rebound tool keys.

### Set coverage and magnet values

The panel shows controls for parts present on the active point:

| Control | Value |
| --- | --- |
| Pit or lava, dirt, and ice width | 1-100 percent coverage; 0 (**Stock**) keeps the stock strip. The **Both** placement divides coverage between both sides. |
| Magnet strength | 0.25-3.00; 0 (**Track**) inherits the track value. |
| Magnet range | 10-200; 0 (**Track**) inherits the track value. Physical reach still has road-width limits. |
| Whole track: lava | **Follow the menu**, **Always kills**, or **Never kills**. Test runs never kill from lava. |
| Whole track: strength and range | The track default for magnets; 0 (**Menu**) uses the Gameplay option. |

Ctrl+click a numeric value to type it. Part settings follow points through insert, delete, reverse,
and path swaps. A part belongs to the segment after its point. **Ignore track settings** in the
Gameplay options bypasses saved lava and magnet values when racing; it does not erase them.
Part widths remain part of the course geometry and still apply.

### Include alternate roads in maps

Enable **Include forks in minimap** in the Course Edit enhancement options or the Content Library
preview. The saved preference affects the race minimap and library preview, not the track file.
It defaults off. The library frames both paths when enabled; its spline preview is an approximation,
not a pixel-identical rendering of the game's map. Race maps keep the game's fixed projection.

### Save and share

Installed forks live in `course-forks.gbs` (GBS1 containing GXB1 blocks). Track and per-point
settings live in `course-settings.gks` (GKS1 containing GXK1 blocks), beside the executable.
Keep these files with the 64DD save sidecar when making a backup.

A `.gdxp` with forks or settings includes a same-stem `.gdxb` (GXD1). This is separate from the
`.gdxe` element-key sidecar described below. A bundle can carry both. Export again after changing
the course so the payload bindings match; copying a stale sidecar is not a migration procedure.

**Compatibility:** 1.2.0 and older refuse packages containing `.gdxb`. GXB1 stays version 1 when
both paths can share their first style. A distinct first style requires version 2; 1.2.0 rejects
and sets aside the whole fork store containing it. Back up the stores before opening them with an
older executable. Package refusal does not protect an already installed save from an older reader.

### Limits to account for

- Junction rendering gives Path A ownership of near-coincident ROAD/BORDERLESS faces in the same
  fork. It clips Path B within 1 world unit only when face normals are nearly parallel
  (absolute dot product at least 0.98). Separated overpasses and steeper crossings stay intact.
  If the retained mesh exceeds a capacity, the whole build keeps the original geometry and logs
  `[fork] junction capacity exceeded; keeping original geometry`; overlap can therefore return.
  This is a rendering change, not a collision or bank-transition repair.
- Path B's interior parts and settings are saved, but its race spans have no gadgets. Do not
  expect pits, lava, magnets, or other placed parts on B to affect a race yet.
- Both paths can have a different first road style; first-point parts and per-point settings
  still share the main-ring point.
- Human lane crossing requires touching or overlapping ROAD or BORDERLESS surfaces. Walled roads
  remain walls, and a crossing that jumps more than one quarter of a lap is refused. Different
  banks can snap the vehicle's orientation. Junction clipping and existing inner-rail flattening
  still need driving and visual acceptance; neither establishes that a reported pop-off is fixed.
- CPUs do not switch paths. Their avoidance uses the part type, not custom coverage or magnet
  range, so a wide hazard can still catch them.
- The control-point limit remains 64. The raised-limit disk/memory migration is not implemented.

The A/B selection, typed Width/Center, optional-map, and junction changes have automated checks,
not runtime acceptance. Verify save/reload and driving on a disposable track before relying on them.

## Bundles (Edit Cup)

A bundle export (`contentType` 3, the six-slot Edit Cup) carries the keys and titles of each member.

- The bundle `.gdxc` keeps its layout. Its flags (header offset 0x25) gain bit 2 (keys) and bit 3 (title) when the companion carries them. The payload CRC at 0x2D does not change.
- The companion is `edit-cup.gdxs`, inside `exports/edit-cup.gdxp`. It is a GXS1 file whose entries are the members, with the id `<slot>.CRSD` and a bind CRC equal to the member's payload CRC. A stale companion is removed when the cup has nothing to carry.
- Member music files travel in the same package, beside the `.gdxc`.
- **Long slot names.** A cup slot holds up to 20 characters. On disk, `CRS_ENTRY.CENT` keeps its stock 24 x 9-byte rows and `CRS_ENTRY.CENL` holds the names over 8 characters (`port/gdx_edit_cup.h`). A bundle carries the CENL image as entry 1 only when the cup has such a name. Older builds refuse those bundles.
- **Old bundles** have no companion and flag bits 2 and 3 clear. They import as before. Their members install with no keys and no title, and any keys a previous install of the same name left behind are cleared.
- **Old builds** ignore the companion and the two flags, and install the bundle as before.

## How import treats a sidecar

A single course can still carry a `.gdxe` sidecar. This is the older form, used by a course export on its own.

- The course always installs from its own data. The sidecar never blocks an import.
- An unknown key plays the fallback stored in the course.
- A sidecar that is missing, damaged, bound to another payload, or names a control point the course does not have is ignored, and the import records why.
- The importer counts unknown keys and keeps the first 16 names.
- Builds without this feature ignore bit 2 and never read the `.gdxe`, so they install the course from its fallbacks.

After an import, the Content Library status line adds these results: whether the sidecar was applied or why it was ignored, and any unknown keys. Code can read them through `gdx_content_import_last_extension`.

### The share package

A course export is one file, `exports/<name>.gdxp`. It is a plain zip of the files below, unchanged,
plus the songs of its custom music keys (`<key suffix>.ogg`, `.flac` or `.wav`, stored uncompressed).
The importer unpacks it into `exports/.gdxp-import`, runs the steps below on the `.gdxc` inside, and
removes the folder. A `.gdxc` with its sidecars beside it, unzipped by hand or from an older build,
still imports the same way.

A package is refused before anything is unpacked when a member is not a plain ASCII file name, has an
unknown extension, or is a Windows device name; when it holds no `.gdxc`, two of them, or a sidecar
with another stem; when two names differ only in case; or when it holds more than 16 files, a data
file over 4 MB or a song over 32 MB. Every member is checked against its zip CRC as it is unpacked.
The code is `port/gdx_content_pack.cpp`, and `gdx_content_pack_tests` covers it.

**Music on import.** A song is copied into `music/custom/` under its key suffix. When that name already
holds a different song, the imported one is stored as `<suffix>_<crc>` (the CRC-32 of its bytes, eight
hex digits) and the course's key is rewritten to match, so neither song replaces the other
(`gdx_course_music_adopt`, `gdx_course_registry_music_fingerprint_key`).

### The sidecar format

A course with extension keys exports as two files inside its package:

- `<name>.gdxc` is the course. Its layout does not change. Its flags (header offset 0x25) get bit 2, value 4, which means a sidecar exists.
- `<name>.gdxe` is the sidecar. It holds the keys, one record for each kind and scope pair.

A course without extension keys has no `.gdxe`, and bit 2 stays clear. It can still have a `.gdxb`
for forks or settings.

The sidecar is bound to its course by the course's payload CRC-32, stored at header offset 0x2D. The importer reads a `.gdxe` only when the `.gdxc` next to it has bit 2 set, and it uses the sidecar only when the CRC matches. A `.gdxe` left in `exports/` by an earlier export therefore does nothing.

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Magic, `GXE1` |
| 0x04 | 2 | Version, 1 |
| 0x06 | 2 | Record count, 0 to 128 |
| 0x08 | 4 | Payload CRC-32 of the `.gdxc` it belongs to |
| 0x0C | varies | Records: kind (1 byte), scope (1 byte), fallback stock ID (1 byte), key length (1 byte), key text |
| end minus 4 | 4 | CRC-32 of every byte before it |

A record's kind is 0 for venue, 1 for skybox, 2 for music, 3 for part style and 4 for track effect. Its scope is 255 (0xFF) for the whole course, or 0 to 63 for one control point. Its fallback is the stock ID the course stores in its own data. The CRC is the same IEEE CRC-32 the `.gdxc` container uses.

## If something goes wrong

**A course plays a stock venue, skybox or song instead of the one it names.**
The build does not know the key, or the sidecar or companion was ignored. The import status says which. Unknown keys are expected when a course moves to a build that lacks the element.

**A pack line is missing from the game.**
The log names the line and the reason (`[elements] ... line N: ...`). Fix that line and restart the game.

**The keys file was moved aside.**
The log says `course-elements.gdxs` was rejected and kept as `course-elements.gdxs.bad`. The store starts empty. The `.bad` file is kept for inspection and is not loaded again.

**The sidecar is ignored.**
The `.gdxe` may have been copied from another export, or the `.gdxc` was exported again after it was made. Export the course again so that both files come from the same export.

**A `.gdxe` sits in `exports/` and does nothing.**
Its `.gdxc` has bit 2 clear, so the importer never reads the sidecar. The course was exported without keys.

**I need more detail.**
The format and the registry API are in `port/gdx_course_registry.h`. The import status type is `GdxContentImportExtension`, in `port/gdx_content_import.h`. The store file is in `port/gdx_course_state.cpp`, and the pack loader in `port/gdx_course_packs.cpp`.

## Quick reference

```text
mods/<pack>/elements.txt   kind|key|name|fallback[|path], one element per line
course-elements.gdxs       GXS1 store of keys and titles, beside the exe
exports/<name>.gdxp        zip of the files below and the course's songs
  <name>.gdxc              flags bit 2 (value 4) set when a sidecar exists
  <name>.gdxe              GXE1 records bound to the .gdxc payload CRC-32 (header offset 0x2D)
  <name>.gdxb              GXD1 forks/settings, when present; needs a newer build than 1.2.0
exports/edit-cup.gdxp      zip of edit-cup.gdxc, its companion and the member songs
  edit-cup.gdxs            GXS1 companion of a bundle, bound to each member's payload CRC-32
```
