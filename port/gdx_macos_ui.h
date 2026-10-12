// port/gdx_macos_ui.h — C-linkage declarations for the macOS-native UI helpers.
// Usable from both C and C++ translation units.
#pragma once

#ifdef __APPLE__

#ifdef __cplusplus
extern "C" {
#endif

// Opens an NSOpenPanel filtered to .z64/.n64/.v64 files. Returns a malloc'd UTF-8 path on
// success, or nullptr if the user cancelled. Caller must free() the returned pointer.
char* gdx_macos_pick_rom(void);

// Shows a modal NSAlert dialog. isError != 0 uses critical style; 0 uses informational.
void gdx_macos_alert(const char* title, const char* message, int isError);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__
