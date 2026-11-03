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
