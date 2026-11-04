# Snapshot compatibility fixtures

Keep these version-1 files unchanged. They verify legacy reading independently of
the version-2 writer in `gdx_course_snapshot_state_tests`.

Both files were generated on 2026-11-03, before the version-2 repair, with
`GdxSnapshotTest_Create` from `gdx_course_snapshot_fixture.c`, disk identity `1`,
directory `0`, course ID `GENERATED.CRSD`, and title `Alpha`. They contain generated
test data, not a player's course or extracted game assets.

| File | Points | SHA-256 |
|------|--------|---------|
| `gdx_course_snapshot_v1_10.gcs` | 10 | `961f34af7b80549131a9f91475fe37aeb5455ecd2a68c140bfa28e478fef9e30` |
| `gdx_course_snapshot_v1_100.gcs` | 100 | `094c53a556faf60a4d610841b1840b6594de01066fbf3205ed2abd7ac3065cee` |

The state tests check byte-exact version-1 re-encoding, native restoration,
version-2 promotion on save, retained history, and portable package round-trips.
Regenerating these fixtures with the version-2 writer would remove that independent
compatibility evidence.

## Version boundary

The `GCS1` magic and 64-byte header remain unchanged. The little-endian version
field at byte offset 4 distinguishes the selector algorithms:

- Version 1 retains the legacy CRC-32 selector for validation.
- Version 2 hashes the same canonical bytes with SHA-256, excluding the final outer
  CRC. It interprets the first four digest bytes as a little-endian 32-bit selector.
  The canonical identity remains disk `1`, directory `0`, and `CONTENT.CRSD`.

Loading version 1 does not migrate it. Sealing a generation writes version 2.
Unknown versions are rejected. Immutable publication still compares the complete
encoded bytes: a remaining 32-bit selector collision refuses the save rather than
overwriting history.

Older binaries cannot read version 2. Restore the matching complete pre-upgrade
save/state backup before downgrading; retaining old snapshots alone is not enough
when the active disk base references a version-2 snapshot.
