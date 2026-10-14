# G-Diffuser — F-Zero X PC source port
# Top-level build (scaffold — fleshed out in milestone M0).
cmake_minimum_required(VERSION 3.20)

project(GDiffuser
    LANGUAGES C CXX
    DESCRIPTION "F-Zero X PC source port"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- Submodule presence check -------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libultraship/CMakeLists.txt")
    message(FATAL_ERROR
        "Submodules not initialized. Run:\n"
        "  git submodule update --init --recursive")
endif()

# Match libultraship's static C runtime (/MT) across the whole tree, or the final link
# fails with a RuntimeLibrary mismatch (LUS = MT_StaticRelease, default = MD_DynamicRelease).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Android ------------------------------------------------------------------
# Configured by Gradle, one CMake tree per ABI (android/app/build.gradle.kts). There is no
# executable: SDLActivity dlopens the game as libmain.so, so port/CMakeLists.txt builds a
# shared library instead. The pieces that only make sense on a desktop -- the standalone
# gdx-extract child process, CPack, the exe-adjacent asset deployment -- are switched off at
# their own sites rather than here.
if(ANDROID)
    # Has to run BEFORE add_subdirectory(libultraship): OVERRIDE_FIND_PACKAGE is what makes
    # the find_package(<name> REQUIRED) calls inside it resolve to the fetched copies rather
    # than failing against a system install the NDK has no equivalent of.
    include(cmake/dependencies/android.cmake)

    # Selects the GLES path in libultraship's Fast3D backend. Set globally, not just on
    # libultraship: port/n64_gfx_bridge.cpp includes the same Fast3D headers and has to agree
    # about which GL it is talking to.
    set(USE_OPENGLES ON CACHE BOOL "" FORCE)
    add_compile_definitions(USE_OPENGLES=1)

    # No TCC for arm64 and nowhere in an APK to dlopen a plugin from, so both only drag in
    # host-only machinery.
    set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
    set(DISABLE_DLL_LOADER ON CACHE BOOL "" FORCE)
endif()
# Fused multiply-add contraction changes rounding in physics code and silently diverges
# from the decomp baseline; MSVC already defaults to no-contraction unless /fp:fast is set.
add_compile_options(
    $<$<OR:$<C_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>
    $<$<OR:$<C_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:AppleClang>>:-ffp-contract=off>
)

# --- Components ---------------------------------------------------------------
add_subdirectory(libultraship)     # runtime + Fast3D renderer
add_subdirectory(port)             # game glue + decomp sources + G-Diffuser executable
#
# Asset extraction: port/CMakeLists.txt builds the standalone `gdx-extract` (a trimmed,
# deterministic Torch, BUILD_STORMLIB=OFF) as an isolated ExternalProject and deploys it
# plus the decomp-recipes/ tree beside the executable. First-boot spawns it against the
# user-supplied ROM to produce generic.o2r. Build directly with:
#   cmake --build <build> --target gdx-extract
#
# Android takes the other branch of that: no second executable, Torch linked in and entered
# through port/android/gdx_extract_entry.cpp, still behind a fork. See docs/ANDROID.md.

message(STATUS "G-Diffuser: libultraship wired via add_subdirectory.")
