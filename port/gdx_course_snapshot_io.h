#ifndef GDX_COURSE_SNAPSHOT_IO_H
#define GDX_COURSE_SNAPSHOT_IO_H

#ifdef __cplusplus
extern "C" {
#endif

int gdx_snapshot_begin(int writing);
void gdx_snapshot_end(void);
int gdx_snapshot_busy(void);
int gdx_snapshot_blocks_writes(void);
int gdx_snapshot_load(unsigned short directory, const char* name, const char* extension,
                      const void* base, void* runtime);
int gdx_snapshot_prepare(unsigned short directory, const char* name, const char* extension,
                         const void* runtime, const void* previous, void* base);
int gdx_snapshot_validate(unsigned short directory, const char* name, const char* extension, const void* base);
struct GdxBranchSet;
int gdx_snapshot_preview(unsigned short directory, const char* name, const void* base,
                         float* x, float* z, int capacity, struct GdxBranchSet* forks);
int gdx_snapshot_alias(unsigned short directory, const char* name, const char* extension, const void* base,
                       unsigned short targetDirectory, const char* targetName, const char* targetExtension);

void gdx_snapshot_export_begin(void);
int gdx_snapshot_export_member(unsigned short directory, const char* name, const char* extension,
                               void* base, int stripGhosts);
int gdx_snapshot_export_write(const char* containerPath, unsigned int containerCrc);
// Hands visit the content bytes (gdx::snapshot::content) of a track for signing. export_content reads the
// index-th member of the export in progress; content reads the stored state of a track, as validate does.
// Returns 1 after visit ran, 0 when there is no such track or its state does not read.
typedef void (*GdxSnapshotContentVisit)(const unsigned char* bytes, unsigned long length, void* ctx);
int gdx_snapshot_export_content(int index, GdxSnapshotContentVisit visit, void* ctx);
int gdx_snapshot_content(unsigned short directory, const char* name, const char* extension, const void* base,
                         GdxSnapshotContentVisit visit, void* ctx);
// gdx_snapshot_content without a single write: no lock file, no snapshot folder, no generation published. A
// track never saved through the snapshot path gets its content bytes built in memory from base. For readers
// that must leave the save untouched, such as a room vote (G-NET decision 8).
int gdx_snapshot_content_peek(unsigned short directory, const char* name, const char* extension, const void* base,
                              GdxSnapshotContentVisit visit, void* ctx);
int gdx_snapshot_package_member(const char* containerPath, unsigned int containerCrc,
                                const char* name, const char* extension, const void* base);
int gdx_snapshot_import_stage(const char* containerPath, unsigned int containerCrc);
int gdx_snapshot_import_check(unsigned short directory, const char* const* names,
                               const void* const* bases, int count);
int gdx_snapshot_import_legacy(unsigned short directory, const char* name, const char* extension,
                               void* base, const void* records, int count, const char* title,
                               const void* forks, const void* settings);
int gdx_snapshot_import_publish(unsigned short directory, const char* name, const char* extension,
                                void* base);
// Stages one track from its content bytes (gdx::snapshot::content), as gdx_snapshot_import_stage stages a
// package: a G-NET room's track the player keeps (plan decision 14). The bytes must be a track's content
// bytes exactly; the track is bound to name with the CRSD extension and sealed, and base (BaseBytes) gets
// the fixed image the disk write saves. gdx_snapshot_import_publish(directory, name, "CRSD", base) then
// takes it. Returns 0 and stages nothing for any other bytes or a name a course id cannot hold.
int gdx_snapshot_import_stage_content(const unsigned char* bytes, unsigned long length, const char* name,
                                      void* base);
int gdx_snapshot_import_restore(unsigned short directory, const char* name, const char* extension,
                                const void* base);

// These adapters run on the native disk fiber; fixed images never alias widened runtime structs.
typedef int (*GdxSnapshotSave)(unsigned short, char*, char*, unsigned char*, unsigned int, int, int, int);
int gdx_snapshot_native_save(GdxSnapshotSave, unsigned short, char*, char*, unsigned char*, unsigned int,
                              int, int, int);
int gdx_snapshot_native_alias(unsigned short entry, const char* newName, const char* newExtension,
                               unsigned short targetDirectory);
int gdx_snapshot_native_backup(unsigned short directory, const char* name, const char* extension);
int gdx_snapshot_native_backup_entry(unsigned short entry);

#ifdef __cplusplus
}
#endif
#endif
