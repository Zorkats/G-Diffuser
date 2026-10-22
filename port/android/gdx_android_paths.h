/* port/android/gdx_android_paths.h — where the app's files live, for code that would
 * otherwise ask the OS where the executable is.
 *
 * Six places in the port resolve their data directory from readlink("/proc/self/exe") (the
 * ROM probe, the disk-image probe, the SRAM migration, first boot, the dump launcher, and the
 * main-module range used by the address resolver). On Android that link is real and useless:
 * it points at /system/bin/app_process64, the zygote, because the game is a shared library the
 * runtime loaded rather than a program the kernel started.
 *
 * So Android answers the question differently, in one place. Both the "executable directory"
 * and the data directory resolve to the app's external files directory —
 * /sdcard/Android/data/<package>/files — which is where the launcher unpacks the recipes, where
 * the archives are built, and the one place a file manager can reach so mods can be dropped in.
 * Everything the desktop layout spreads across an install directory and a data directory sits
 * together there, which is also what makes the port's exeDir/dataDir distinction collapse
 * harmlessly on this platform.
 *
 * The main-module range is the one caller this does NOT serve: it needs the mapping of
 * libmain.so, not a directory. See the Android branch in port/n64_gfx_bridge.cpp.
 */
#ifndef GDX_ANDROID_PATHS_H
#define GDX_ANDROID_PATHS_H

#ifdef __ANDROID__

#ifdef __cplusplus
extern "C" {
#endif

/* The app's external files directory, with NO trailing separator. Never NULL and never empty:
 * if SDL cannot answer (which would mean it has not been initialized, and the game is running
 * before SDL_main), it falls back to the current working directory, which the launcher has
 * already set to that same place. */
const char* gdx_android_files_dir(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __ANDROID__ */
#endif /* GDX_ANDROID_PATHS_H */
