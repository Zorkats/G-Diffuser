#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int gdx_recovery_blocks_writes(void);
int gdx_recovery_persistence_busy(void);
void gdx_recovery_disk_ready(void);
void gdx_recovery_tick(void);
void GdxContentRecovery_RunOnDiskThread(void);
int gdx_recovery_write_await(const char* path, const uint8_t* bytes, size_t size);
int gdx_recovery_native_read(const char* name, const char* ext, uint8_t* out, size_t cap, size_t* size);
int gdx_recovery_native_write(const char* name, const char* ext, const uint8_t* bytes, size_t size);
int gdx_recovery_native_cups(uint8_t* cent, uint8_t* cenl);
int gdx_recovery_native_cup_slot(int cenl, int slot, const uint8_t* bytes, size_t size);
unsigned gdx_recovery_native_directory(void);
int gdx_recovery_native_prepare(void);
int gdx_recovery_native_restore_ready(const char* name, const char* ext, const uint8_t* bytes,
                                     size_t size, int cupMask);
void* gdx_recovery_native_thread(void);
#ifdef __cplusplus
}
struct GdxContentEntry;
bool GdxRecovery_Draw(const GdxContentEntry* selected);
#endif
