// port/android/gdx_extract_entry.cpp — the extractor's entry point, on Android.
//
// On every other platform `gdx-extract` is a separate executable built out-of-tree
// (port/CMakeLists.txt), and first boot spawns it. An APK has nowhere to put a second
// executable — native code is loaded from the app's library directory as shared objects, and
// W^X has blocked exec() out of app-writable storage since API 29 — so Torch is linked into
// libmain.so instead and reached through this function.
//
// It is Torch's own main(), renamed, and NOT a reimplementation of it. The launcher in
// port/gdx_extract_launch.cpp hands over the same argv it would have handed the child process
// (`o2r`, `ipl`, `disk`, `dump` with the same flags), so the two platforms have to agree about
// what those arguments mean. Including the source is what keeps that true without a second
// copy of the CLI to drift out of step.
//
// STANDALONE is what un-guards main() in that file. It is defined for THIS translation unit
// only: the rest of Torch is compiled without it (USE_STANDALONE=OFF), which drops the
// ExportType::Code exporters — C-code generation, which needs libgfxd and which nothing on a
// phone asks for. The binary/O2R exporters, the only ones the launcher drives, are unaffected.
#ifdef __ANDROID__

// Declared with C linkage BEFORE the definition is seen, so the definition inherits it. Without
// this the renamed main() is an ordinary C++ function and the launcher's extern "C" declaration
// looks for a symbol that does not exist -- a link error that names neither file.
extern "C" int gdx_extract_main(int argc, char* argv[]);

// Renames Torch's entry point before its definition is seen. The launcher calls it from the
// child of a fork(), so it still owns a whole process: exit codes, stdout, and the extraction
// peak all behave exactly as they do around the desktop child.
#define main gdx_extract_main
#define STANDALONE 1

#include "../../torch/src/main.cpp"

#undef main

#endif // __ANDROID__
