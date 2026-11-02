/* port/disk_savefile.h -- durable 64DD disk-save sidecar (copy-on-write dirty-LBA journal).
 *
 * The port's 64DD "drive" is a linear disk image loaded into gdx_disk_buffer
 * (port/disk_buffer.cpp). Game writes (Course Edit saves, MFS RAM-area formats,
 * ghost autosaves that land on disk) update that in-memory image; this module
 * makes them durable WITHOUT ever mutating the user's pristine .ndd, by recording
 * only the dirty byte ranges into a sidecar next to the executable
 * ("saves/<diskFileName>.gdd") and replaying them at the next boot.
 *
 * Design: copy-on-write journal. The pristine .ndd is the immutable base; the
 * sidecar is a coalesced list of {byteOffset, length, data} records covering
 * exactly the ranges the game has written since the base was authored. A
 * CRC64 fingerprint of the pristine image is stored in the sidecar header so a
 * sidecar is NEVER applied to a disk it was not created against.
 *
 * Host-CRT translation unit (G-Diffuser executable target, like port/sram_buffer.cpp
 * and port/gdx_ghost_io.c), so the standard file API is available here. The decomp
 * game object library never sees fopen/FILE; it reaches the two decomp-facing entry
 * points (gdx_disk_save_mark_dirty from port/n64_leo.c's write path, and the format
 * guard below) through raw C externs, the same boundary idiom sram_buffer.cpp uses.
 */
#ifndef GDX_DISK_SAVEFILE_H
#define GDX_DISK_SAVEFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-64/XZ (ECMA-182 polynomial, reflected 0xC96C5795D7870F42), the same implementation
 * gdx_disk_save_init uses internally to fingerprint the pristine disk. Exposed so other host
 * TUs (port/disk_buffer.cpp's EK disk-variant detection) get an identical fingerprint without
 * a second copy of the table. */
unsigned long long gdx_disk_crc64(const unsigned char* data, unsigned long long length);

/* Fingerprints the pristine disk, resolves the sidecar path (saves/<diskName>.gdd next
 * to the exe) and loads any existing sidecar into the in-memory journal. `pristine` must
 * point at the freshly loaded, UNMODIFIED disk bytes -- the fingerprint is taken from
 * them. `diskName` is the base file name of the loaded .ndd; the sidecar mirrors it. On
 * validation failure the loader falls back .gdd then .gdd.bak then pristine-only; a
 * mismatched sidecar is ignored. */
void gdx_disk_save_init(const char* diskName, const unsigned char* pristine, unsigned int size);

/* Replay the loaded journal records over `buffer` (the freshly loaded pristine
 * image, i.e. gdx_disk_buffer). No-op when the journal is empty. */
void gdx_disk_save_apply(unsigned char* buffer);

/* Record a dirty byte range written to the disk image. Coalesces overlapping
 * and adjacent ranges. Arms the debounced flush (see gdx_disk_save_tick). */
void gdx_disk_save_mark_dirty(unsigned int offset, const void* data, unsigned int len);

/* Atomically persist the current journal to saves/<name>.gdd (temp + rename,
 * rolling the previous file to .gdd.bak). Normally driven by the debounce tick;
 * exposed for explicit/forced flushes. */
void gdx_disk_save_flush(void);
unsigned long long gdx_disk_save_identity(void);
int gdx_disk_save_recovery_flush(void);
int gdx_disk_save_recovery_path(char* out, unsigned int capacity);

/* Per-host-frame debounce tick. Flushes the journal once a write burst has
 * drained (no new mark_dirty for a short window), coalescing one game save into
 * one atomic sidecar write. Call once per frame from the host loop. */
void gdx_disk_save_tick(void);

/* Host format guard. Non-zero only when a disk-formatting write is permitted; default
 * FALSE, because an unprompted auto-format of an uninitialized or foreign MFS RAM area
 * would overwrite the user's prior sidecar content. Called from the decomp EK format
 * sites -- see the terminal-only-consumption rule in disk_savefile.cpp. */
int gdx_disk_allow_format(void);

/* Always-on, rate-limited refusal log for the D6 format guard. Callable from the
 * decomp EK translation units (which cannot include the host logging header). */
void gdx_disk_log_format_refused(void);

/* Workshop-menu status getters. All are cheap and safe to call every frame from the
 * ImGui draw. */

/* Non-zero when a valid sidecar (.gdd or its .bak) was loaded this boot. */
int gdx_disk_sidecar_present(void);

int gdx_disk_sidecar_record_count(void);

/* Non-zero when the most recent flush attempt succeeded; 1 initially, since a fresh
 * journal with nothing to flush is "not failed". */
int gdx_disk_last_flush_ok(void);

/* Non-zero when the MFS RAM area was found uninitialized AND the format guard refused
 * at least once this boot -- i.e. the "Initialize DD save area" button should be
 * offered. */
int gdx_disk_format_refused_this_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_DISK_SAVEFILE_H */
