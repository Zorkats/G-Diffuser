// port/android/gdx_android_paths.cpp — see gdx_android_paths.h.
#ifdef __ANDROID__

#include "gdx_android_paths.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>

#include <mutex>
#include <string>
#include <unistd.h>

namespace {

// Resolved once. SDL_AndroidGetExternalStoragePath() returns a pointer into SDL's own storage
// that stays valid for the life of the process, but it is also documented to be able to fail,
// and callers here want a string they can hand to std::filesystem without checking.
std::string ResolveFilesDir() {
    if (const char* fromSdl = SDL_AndroidGetExternalStoragePath(); fromSdl != nullptr && fromSdl[0] != '\0') {
        std::string path = fromSdl;
        while (path.size() > 1 && path.back() == '/') {
            path.pop_back();
        }
        return path;
    }

    // SDL cannot answer. In the game process that does not happen -- SDL_Android_Init runs
    // before SDL_main, and the first caller here is first boot at the top of it -- so this is
    // reached only when the library was loaded WITHOUT the SDL activity behind it, which is
    // what the launcher process does to reach a handful of JNI calls. Everything the launcher
    // asks for is passed an explicit directory, so nothing there depends on this answer; the
    // working directory is returned rather than an empty string only so the result is always a
    // usable path.
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        return std::string(cwd);
    }
    return std::string(".");
}

} // namespace

extern "C" const char* gdx_android_files_dir(void) {
    static const std::string* dir = new std::string(ResolveFilesDir());
    return dir->c_str();
}

#endif // __ANDROID__
