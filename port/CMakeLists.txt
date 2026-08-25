# port/ — game glue + decomp game sources compiled for the host. Kept standalone-capable so the
# decomp compile can iterate without rebuilding libultraship.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(gdiffuser_port LANGUAGES C CXX)
endif()

set(DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GDX_DISPLAY_VERSION "1.1.0" CACHE STRING "Version shown in the About menu")

# Warning flags for the port's own targets under GCC/Clang; the decomp library and vendored cxd4
# stay -w (not ours to fix). No -Werror: a Linux first-configure should surface warnings, not fail.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(GDX_PORT_WARN_FLAGS -Wall -Wno-unused-parameter)
else()
    set(GDX_PORT_WARN_FLAGS "")
endif()

# Decomp game C compiled for PC. libultra/* is excluded (libultraship provides it) except gu/
# (matrix math) and os/ — we run the decomp's own cooperative thread scheduler and supply only
# the low-level context switch in the port. rsp/* and leo/* are handled elsewhere.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${DECOMP}/src/game/*.c"
    "${DECOMP}/src/sys/*.c"
    "${DECOMP}/src/overlays/*.c"
    "${DECOMP}/src/audio/*.c"
    "${DECOMP}/src/framebuffers/*.c"
    "${DECOMP}/src/libultra/gu/*.c"
    "${DECOMP}/src/libultra/os/*.c"
)

# Time/timers come from libultraship (os_time.cpp) — exclude the decomp's to avoid duplicate
# symbols. Threading/messaging/events stay decomp.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/gettime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settimer\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/timerintr\\.c$")
# Our osCreateThread (port/n64_sched.c) captures entry+arg as full 64-bit pointers; the decomp's
# truncates them into u32 context fields.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/createthread\\.c$")
# The decomp's osInitialize does real PIF/PI register I/O — replaced in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/initialize\\.c$")
# The decomp's osGetMemSize dereferences N64 bus addresses — replaced in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/getmemsize\\.c$")

# Expansion Kit: EXPANSION_KIT activates the decomp's EK hooks WITHOUT VERSION_JP — the
# VERSION_JP sites are JP-cart text/font differences deliberately kept on US behavior.
# EK modes are runtime-gated by disk-image presence (port/n64_leo.c).
option(GDX_EXPANSION_KIT "Compile Expansion Kit (64DD) support into the port" ON)

# GDX_DEV_TOOLS — master switch for the behaviour-altering (Bucket B) developer gates
# (port/gdx_dev_gates.h, docs/menu/DEVELOPER_TAB.md). When absent, every Bucket B gate is a
# hard-wired 0 (no getenv, no CVar read) and the Dev Tools page omits the section — safe because
# the gate table normalizes 0 == stock behaviour. ON everywhere except Release; the generator
# expression keeps one build tree correct under both single- and multi-config generators.
# GDX_FORCE_DEV_TOOLS keeps the gates in a Release soak/QA binary.
option(GDX_FORCE_DEV_TOOLS "Keep the behaviour-altering developer gates in Release builds too" OFF)
if(GDX_FORCE_DEV_TOOLS)
    set(GDX_DEV_TOOLS_DEFS "GDX_DEV_TOOLS=1")
else()
    set(GDX_DEV_TOOLS_DEFS "$<$<NOT:$<CONFIG:Release>>:GDX_DEV_TOOLS=1>")
endif()

# GDX_ALLOW_JP_INPUTS — whether the ORDINARY (US) binary may ingest Japanese-region game data.
# Not GDX_BUILD_JP (that builds a second VERSION_JP executable).
#
# Defaults OFF because the JP raw-ROM path plays through US-profile binding tables and produces
# loud static/blasting audio (observed, owner testing) — an ear-safety hazard, so release builds
# refuse JP inputs outright. Gated, not deleted; JP support is a later release.
# Covered sites: gdx_firstboot.cpp (refuses JP ROM by SHA-1 and header country code, and the
# retail JP EK disk), rom_buffer.cpp (runtime cartridge loader), disk_buffer.cpp (drops JP disk
# filenames, refuses the retail JP image by CRC-64). The 64DD IPL is deliberately NOT gated: the
# drive firmware is Japanese by existence and carries no game audio.
# The G-Diffuser-JP target always sets it — building that target is the opt-in.
option(GDX_ALLOW_JP_INPUTS
       "Accept Japanese-region game data (JP cartridge ROM, retail JP Expansion Kit disk). DEV ONLY — the JP raw-ROM path has broken audio." OFF)
if(GDX_ALLOW_JP_INPUTS)
    message(STATUS "G-Diffuser: GDX_ALLOW_JP_INPUTS=ON — Japanese ROM/disk inputs are ACCEPTED (experimental; JP audio is known-bad).")
    set(GDX_JP_INPUT_DEFS "GDX_ALLOW_JP_INPUTS=1")
else()
    # Empty LIST, not an empty string element, so the unquoted expansion contributes no argument.
    set(GDX_JP_INPUT_DEFS)
endif()

if(NOT GDX_EXPANSION_KIT)
    # Mirrors the decomp Makefile's EXPANSION_KIT=0 EXCLUSION_FILES list.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/disk/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/dd_save\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/ovl_i2_data2\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i10/.*187510\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/expansion_kit/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/course_edit/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/machine_create/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/")
else()
    # Mirrors the decomp Makefile's EXPANSION_KIT=1 exclusions: the disk-side subsystems REPLACE
    # their rom-side twins (audio/disk vs audio/rom, sys/disk vs sys/rom, dd_save.c vs
    # save_buffer.c) — alternatives, never compiled together.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/save_buffer\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i11/")
    # ead_demo (64DD attract demo) has broken WIP C bodies upstream — exclude until the decomp
    # matures; cosmetic-only.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
    # libultraship provides the osAi* surface (same reason as aisetnextbuf.c below).
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/lib/os\\.c$")
    # Only the pure-math leo files (LBA<->byte zone tables); the hardware command/interrupt
    # layer is replaced wholesale by port/n64_leo.c.
    file(GLOB EK_LEO_SOURCES
        "${DECOMP}/src/leo/lib/bytetolba.c"
        "${DECOMP}/src/leo/lib/lbatobyte.c"
        "${DECOMP}/src/leo/lib/leo_tbl.c"
        "${DECOMP}/src/leo/lib/leotranslat.c"
        "${DECOMP}/src/leo/lib/leoutil.c"
        "${DECOMP}/src/leo/mfs/*.c"
    )
    list(APPEND GAME_SOURCES ${EK_LEO_SOURCES})
endif()

# libultraship already provides these libultra functions — exclude to avoid duplicate symbols.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/lib/aisetnextbuf\\.c$")  # osAiSetNextBuffer

# segment.c / cartridge_offsets.c: N64 ROM-segment system — linker segment symbols don't exist
# on host; replaced by libultraship resource loading.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/segment\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/virtualtophysical\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/physicaltovirtual\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")

# ovl_i2/save.c is deliberately NOT excluded: Sram_Init/Sram_ReadWrite are PORT-ified in place to
# hit the host-backed SRAM image (port/sram_buffer.cpp). Save_LoadStaffGhostRecord stays stubbed,
# pending the EK ROM segment table.

add_library(gdiffuser_game OBJECT ${GAME_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"
    "${DECOMP}/src/libultra/io/devmgr.c"                  # outside the glob: PORT PI manager, host-backed DMA completion
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/AssetBindings.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/LinkStubs.c")         # placeholder defs so bring-up links

if(GDX_EXPANSION_KIT)
    # The decomp directly includes generated zero-filled storage definitions. Keep generation in
    # the build graph so a clean or partially cleaned tree cannot compile before they exist. The
    # compile-only mode is load-bearing: the full generator rewrites curated runtime binding sizes.
    add_custom_target(gdiffuser_ek_compile_assets
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_ek_assets.py"
                --compile-assets-only
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        COMMENT "Generating Expansion Kit compile assets"
        VERBATIM)
    add_dependencies(gdiffuser_game gdiffuser_ek_compile_assets)

    target_sources(gdiffuser_game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_leo.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkAssetBindings.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkLinkStubs.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_ek_disk_overrides.c")
    # MFS_VERSION=1 and BUILD_VERSION=VERSION_J (2.0J libultra API shapes) match the decomp
    # Makefile's EK configuration; sethwinterrupt.c and the headers are gated on it.
    target_compile_definitions(gdiffuser_game PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
endif()

# Fiber backend for the scheduler (n64_sched.c -> gdx_fiber.h): Win32 fibers on Windows,
# POSIX ucontext elsewhere.
if(WIN32)
    target_sources(gdiffuser_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_win32.c")
else()
    target_sources(gdiffuser_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_ucontext.c")
endif()

target_include_directories(gdiffuser_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
)

target_compile_definitions(gdiffuser_game PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    PORT=1
    GDIFFUSER_PORT=1
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1
    ASSET_VERSION=us       # used by ASSET_HEADER() to build include paths
    ASSET_REVISION=rev0
)

# MSVC: silence IDO-era decomp noise, but keep fatal the classes that are UB on x64 and have
# caused real crashes: C4715/C4716 (non-void falls off the end — MIPS left the value in $v0,
# MSVC returns garbage) and C4013 (implicit decl truncates a 64-bit pointer return to EAX;
# Course Edit's "Test Course" crashed this way for want of one prototype).
# They MUST be spelled /we, not /w1NNNN: /W0 outranks /w1NNNN and would silence them again. /we
# is independent of the warning level.
if(MSVC)
    target_compile_options(gdiffuser_game PRIVATE /W0 /we4715 /we4716 /we4013)
else()
    # GCC/Clang: -w like /W0, but -Werror=return-type keeps the falls-off-the-end class fatal
    # (same x64 crash source as /we4715). GCC 14+ promotes int-conversion /
    # incompatible-pointer-types / implicit-function-declaration to default ERRORS that -w does
    # not downgrade; the IDO-era decomp mixes pointers and 32-bit ints freely, so downgrade those
    # back to silenced warnings. -fcommon: the generated bindings and decomp asset TUs carry
    # tentative definitions of the same symbols (pre-GCC-10/MSVC merged them; GCC 10+ hard-errors).
    # -fno-strict-aliasing / -fwrapv are LOAD-BEARING at -O2/-O3: the decomp type-puns constantly
    # and assumes wrapping arithmetic; TBAA miscompiled the Release audio command path (near-silent
    # audio, ai-tap rms 482 vs Debug's 2498) while -O0 was healthy. Keep on ALL build types.
    target_compile_options(gdiffuser_game PRIVATE -w -Werror=return-type -fcommon
        -fno-strict-aliasing -fwrapv
        -Wno-error=int-conversion
        -Wno-error=incompatible-pointer-types
        -Wno-error=implicit-function-declaration)
endif()

# cxd4 RSP interpreter (LLE audio), vendored under rsp/cxd4 (CC0). Static lib so its C89 sources
# get isolated quiet flags and its module-level globals stay out of the game TUs. Driven by
# gdx_audio_lle.c via gdx_rsp_driver.h. gdx_rsp_boot_tests compiles its own copy, so this lib
# is game-only.
add_library(cxd4_rsp STATIC
    rsp/cxd4/gdx_rsp_driver.c
    rsp/cxd4/su.c
    rsp/cxd4/vu/add.c
    rsp/cxd4/vu/divide.c
    rsp/cxd4/vu/logical.c
    rsp/cxd4/vu/multiply.c
    rsp/cxd4/vu/select.c
    rsp/cxd4/vu/vu.c)
target_include_directories(cxd4_rsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rsp/cxd4")
# SP_EXECUTE_LOG enables cxd4's per-instruction hook so the driver's watchdog
# (gdx_rsp_driver.c step_SP_commands) can longjmp out of a runaway/mis-marshalled task
# instead of spinning the audio thread forever.
target_compile_definitions(cxd4_rsp PRIVATE SP_EXECUTE_LOG=1)
if(MSVC)
    target_compile_options(cxd4_rsp PRIVATE /W0)
    target_compile_definitions(cxd4_rsp PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    # Quiet like /W0; -fno-strict-aliasing / -fwrapv for the same miscompile reason as
    # gdiffuser_game above (the RSP core type-puns DMEM/registers and relies on wraparound).
    target_compile_options(cxd4_rsp PRIVATE -w -fno-strict-aliasing -fwrapv)
    target_compile_definitions(cxd4_rsp PRIVATE _CRT_SECURE_NO_WARNINGS=1)
endif()

# Discord Rich Presence: vendored static discord-rpc (see its CMakeLists for provenance and the
# strip rationale). EXCLUDE_FROM_ALL keeps it out of the test targets' builds.
add_subdirectory(third_party/discord-rpc EXCLUDE_FROM_ALL)

# Lua 5.4.8: vendored static lib for the scripting layer. Dormant for now — linked by both
# executables but no port code references it yet (the runtime core lands separately).
add_subdirectory(third_party/lua EXCLUDE_FROM_ALL)

add_executable(G-Diffuser
    main.cpp
    gdx_discord.cpp
    gdx_gui.cpp
    gdx_menu.cpp
    gdx_menu_registry.cpp
    gdx_console_log.cpp
    ui/UIWidgets.cpp           # ported from HarbourMasters/Lighthouse, CC0 1.0
    enhancements/events/GameEvents.cpp      # REQUIRED even with every enhancement off: decomp
                                # racer.c references GameEvents_FireOnBoostStart, so dropping this
                                # is a link failure, not a missing feature.
    enhancements/tuning/BoostDuration.cpp   # self-registers from a static initializer; nothing
                                # references it, so it looks like dead code and is not.
    input_bridge.c
    gdx_course_edit_mouse.cpp  # Course Edit absolute mouse drive + issue #18 steering; extern "C"
                               # wrappers over Fast3dWindow for the decomp-side C callers
    gdx_input_script.c
    n64_gfx_bridge.cpp
    gdx_interp.cpp
    n64_gfx_convert.cpp
    gdx_vi_convert.c
    n64_audio_hle.c            # standalone C, deliberately not part of gdiffuser_game
    gdx_audio_lle.c
    gdx_audio_thread.cpp
    gdx_audio_capture.c
    gdx_frame_pacer.c          # default OFF: LUS's Fast3D backend already paces EndFrame() to 60fps
    shims.c
    AssetLoader.cpp
    resource/ResourceFactories.cpp
    gdx_firstboot.cpp
    gdx_extract_launch.cpp
    gdx_dump_launch.cpp
    gdx_firstboot_gui.cpp
    rom_buffer.cpp
    sram_buffer.cpp
    disk_savefile.cpp
    gdx_ghost_io.c
    gdx_ghost_window.cpp
    gdx_content_io.c               # E1 .gdxc export; mirrors MFS dir entries like gdx_ghost_io.c
                                   # mirrors the ghost structs, walking gMfsRamArea directly
    gdx_content_import.c           # E2 .gdxc import; validation chain + disk-thread install
                                   # staging (op slot 30 in decomp/src/sys/disk/75000.c)
    gdx_content_window.cpp         # Content Library window; the F12 unified Export/Import shell
    gdx_xcup_seed.c                # M3 X Cup seed codes; hooked from Course_GenerateRandomCourse
    gdx_palette.cpp              # F2 palette editor; mirrors Machine like gdx_ghost_io.c mirrors
                                 # the ghost structs, so it links against racer.c without decomp headers
    gdx_custom_grid.c            # F3 custom grid; roster-string CVar parse/format, called from
                                 # func_80089800 (racer.c) under #ifdef PORT
    gdx_achievements.cpp         # F10 playtime + local achievements; mirrors SaveContext layout
                                 # with static_asserts, polls gSaveContext, persists to saves/
    gdx_input_viewer.cpp
    gdx_fps_overlay.cpp
    gdx_perf.cpp
    gdx_dev_gates.c            # dependency-free C: CVar entry points arrive as function pointers
                                # from main.cpp, so the standalone test targets compile it unchanged
    gdx_imgui_nav.cpp
    gdx_workshop.cpp
    gdx_audio_seq_packs.cpp      # stage-2 audio modding; hooks live in decomp/src/audio/*/lib/load.c
    gdx_audio_sample_packs.cpp   # stage-3 audio modding; hook lives in decomp/src/audio/disk/lib/load.c
    gdx_audio_soundfont_packs.cpp # stage-4 audio modding; hooks live in decomp/src/audio/disk/lib/load.c
    gdx_lua.cpp                  # workshop Lua script packs; the decomp C shim is gdx_lua_state.c
    gdx_lua_state.c              # includes unk_structs.h directly (exe targets carry the decomp
                                 # include dirs) so gdx_lua.cpp stays decomp-header-free
    gdx_model_packs.cpp          # F4a stock-machine model packs; hooks live in decomp/src/game/racer.c
    mio0_wrap.c
    gdx_segment_source.c       # in the exe, not gdiffuser_game: needs symbols from AssetLoader.cpp,
                                # rom_buffer.cpp and gen/AssetBindings.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
    $<TARGET_OBJECTS:gdiffuser_game>)

if(GDX_EXPANSION_KIT)
    # All four files reference EK-only symbols (gdx_ek_assets_fill, machine_create labels, EK
    # overlay .data), so they must not build in US-only configs.
    target_sources(G-Diffuser PRIVATE disk_buffer.cpp gen/EkTranslatedOverrides.c
                                      gdx_ek_strings.c gen/EkTranslatedStrings.c)
    # shims.c guards its US-only Leo stubs on this define.
    target_compile_definitions(G-Diffuser PRIVATE EXPANSION_KIT=1)
endif()

target_compile_definitions(G-Diffuser PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    ${GDX_JP_INPUT_DEFS}
    GDX_VERSION_STRING="${GDX_DISPLAY_VERSION}")

if(WIN32)
    set_target_properties(G-Diffuser PROPERTIES WIN32_EXECUTABLE TRUE)
    # Application icon; Linux desktop entries carry theirs separately.
    target_sources(G-Diffuser PRIVATE gdiffuser.rc)
    # Emit a linker .map so crash RVAs and low32 data addresses in gdiffuser-run.log resolve to
    # symbols without a debugger.
    #
    # DO NOT add /DYNAMICBASE:NO. It was tried, to close the ASLR low32-collision class (a random
    # image base whose low32 lands in an N64 address window misroutes reconstructed 32-bit tokens
    # — see the KSEG guard in decomp/src/sys/dma.c), and the fixed base 0x140000000 was worse:
    # module low32s at 0x40000000..0x41FFFFFF corrupted the translated menu/text textures
    # build-wide and null-write-crashed Fast::Interpreter::ImportTextureRgba32 at startup
    # (GDX_WIDE_ASSET_RESOLVE=0 ruled out the resolver, leaving the base). Until every low32
    # window is audited against a chosen base, a fixed base is a bigger dice roll than ASLR.
    if(MSVC)
        target_link_options(G-Diffuser PRIVATE "/MAP")
    endif()
endif()

# Scrubs a game-derived icon that older build trees still carry; harmless on a clean tree.
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:G-Diffuser>/gdiffuser.png"
    VERBATIM)

file(GLOB_RECURSE GDIFFUSER_F3D_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/libultraship/src/fast/shaders/*")
# gdiffuser.o2r carries only the MIT-licensed Fast3D shaders.
set(GDIFFUSER_ENGINE_O2R "${CMAKE_CURRENT_BINARY_DIR}/gdiffuser.o2r")
add_custom_command(
    OUTPUT "${GDIFFUSER_ENGINE_O2R}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_f3d_o2r.py" "${GDIFFUSER_ENGINE_O2R}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_f3d_o2r.py"
            "${CMAKE_SOURCE_DIR}/assets/branding/gdiffuser-logo-menu.png"
            ${GDIFFUSER_F3D_SHADER_SOURCES}
    COMMENT "Generating engine archive gdiffuser.o2r (Fast3D shaders)"
    VERBATIM)
add_custom_target(gdiffuser_engine_o2r DEPENDS "${GDIFFUSER_ENGINE_O2R}")
add_dependencies(G-Diffuser gdiffuser_engine_o2r)
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDIFFUSER_ENGINE_O2R}" "$<TARGET_FILE_DIR:G-Diffuser>/gdiffuser.o2r"
    VERBATIM)

# Fonts stay loose beside the exe so GdxFast3dGui can add them to ImGui's atlas before the
# renderer uploads it.
set(GDIFFUSER_GUI_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:G-Diffuser>/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDIFFUSER_GUI_FONT_DIR}/Montserrat-Regular.ttf"
            "${GDIFFUSER_GUI_FONT_DIR}/Inconsolata-Regular.ttf"
            "${GDIFFUSER_GUI_FONT_DIR}/Montserrat-OFL.txt"
            "${GDIFFUSER_GUI_FONT_DIR}/Inconsolata-OFL.txt"
            "$<TARGET_FILE_DIR:G-Diffuser>/fonts"
    VERBATIM)

# SDL only knows the pads that existed when it was released, and the one we bundle is pinned.
# The community database is updated continuously and SDL_GameControllerAddMappingsFromFile layers
# it on top, so a pad newer than the bundled SDL still works. It has to sit beside the exe under
# this exact name: LocateFileAcrossAppDirs resolves it from the bare string in os.cpp.
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/gamecontrollerdb.txt"
            "$<TARGET_FILE_DIR:G-Diffuser>/gamecontrollerdb.txt"
    VERBATIM)

# Keep the project license and every bundled/static dependency notice with binary releases.
set(GDIFFUSER_LICENSE_DIR "${CMAKE_SOURCE_DIR}/LICENSES")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:G-Diffuser>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${GDIFFUSER_LICENSE_DIR}"
            "$<TARGET_FILE_DIR:G-Diffuser>/LICENSES"
    VERBATIM)

# gdx-extract, the standalone deterministic O2R extractor first boot spawns as a child process.
# Isolated ExternalProject, NOT add_subdirectory'd:
# Torch pulls its own spdlog/fmt/yaml-cpp via FetchContent, and out-of-tree prevents ODR/version
# clashes with libultraship's spdlog/fmt (docs/investigation/.../W0_TORCH_DEPENDENCIES.md).
# GDX_DETERMINISTIC=ON => byte-identical archives for identical ROM + recipes. CMP0091=NEW lets
# Torch's /MT runtime take effect so the exe needs no VC++ redist.
include(ExternalProject)

set(GDX_EXTRACT_SRC_DIR "${CMAKE_SOURCE_DIR}/torch")
set(GDX_EXTRACT_PREFIX  "${CMAKE_BINARY_DIR}/gdx-extract")
set(GDX_EXTRACT_INSTALL "${GDX_EXTRACT_PREFIX}/install")
set(GDX_EXTRACT_EXE     "${GDX_EXTRACT_INSTALL}/bin/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}")

ExternalProject_Add(gdx_extract_ext
    SOURCE_DIR  "${GDX_EXTRACT_SRC_DIR}"
    PREFIX      "${GDX_EXTRACT_PREFIX}"
    INSTALL_DIR "${GDX_EXTRACT_INSTALL}"
    CMAKE_CACHE_ARGS
        -DUSE_STANDALONE:BOOL=ON
        -DBUILD_STORMLIB:BOOL=OFF
        -DGDX_DETERMINISTIC:BOOL=ON
        -DGDX_OUTPUT_NAME:STRING=gdx-extract
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
        -DCMAKE_POLICY_DEFAULT_CMP0091:STRING=NEW
    BUILD_COMMAND   "${CMAKE_COMMAND}" --build   <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config $<CONFIG>
    # BUILD_ALWAYS ON: ExternalProject stamps do not track torch/ source changes, so OFF served a
    # stale extractor from the install prefix (bit us twice). Incremental no-change rebuild is
    # a few seconds; correctness wins.
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${GDX_EXTRACT_EXE}")

add_custom_target(gdx-extract DEPENDS gdx_extract_ext)
add_dependencies(G-Diffuser gdx_extract_ext)

add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_EXTRACT_EXE}"
            "$<TARGET_FILE_DIR:G-Diffuser>/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}"
    VERBATIM)

# Deploy the recipe tree as decomp-recipes/ next to the exe. The packaged layout mirrors the
# decomp source tree verbatim so config.yml needs no edits (Torch resolves paths relative to it).
# Only us/rev0 ships; other revisions' config keys are never dereferenced because recipe
# selection is by matched ROM SHA1.
set(GDX_RECIPES_SRC "${CMAKE_SOURCE_DIR}/decomp")
set(GDX_RECIPES_DST "$<TARGET_FILE_DIR:G-Diffuser>/decomp-recipes")
# The EK slice manifest ships beside the recipes so the runtime disk step can pass it to
# `gdx-extract disk -m`.
set(GDX_EK_SLICE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/gen/ek_slice_manifest.txt")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GDX_RECIPES_DST}/assets/yaml/us"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_RECIPES_SRC}/config.yml"
            "${GDX_RECIPES_DST}/config.yml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_RECIPES_SRC}/LICENSE"
            "${GDX_RECIPES_DST}/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_EK_SLICE_MANIFEST}"
            "${GDX_RECIPES_DST}/ek_slice_manifest.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${GDX_RECIPES_SRC}/assets/yaml/us/rev0"
            "${GDX_RECIPES_DST}/assets/yaml/us/rev0"
    # Dump All data files, generated from the single source of truth at build time: audio tables
    # from decomp audio_tables.c and the EK TLUT map from the EK recipe tree, so the dump classes
    # work on machines without decomp source.
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_dump_tables_data.py"
            --audio-tables "${GDX_RECIPES_SRC}/src/audio/disk/audio_tables.c"
            --out-dir "${GDX_RECIPES_DST}"
            --ek-yaml-dir "${CMAKE_SOURCE_DIR}/fzerox-expansion-kit/assets/yaml/jp"
    VERBATIM)

target_include_directories(G-Diffuser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"   # for "resource/ResourceFactories.h"
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0"  # mio0.h + utils.h for mio0_wrap.c and mio0.c
)

target_link_libraries(G-Diffuser PRIVATE cxd4_rsp)
target_link_libraries(G-Diffuser PRIVATE discord-rpc)
target_link_libraries(G-Diffuser PRIVATE gdx_lua)

if(WIN32)
    # user32: MessageBox; Comdlg32: ROM picker; Comctl32: extraction progress marquee.
    target_link_libraries(G-Diffuser PRIVATE user32 Comdlg32 Comctl32)
else()
    # pthread for the fiber backend + audio thread; dl for dladdr in n64_gfx_bridge.cpp
    # (needed on older glibc, harmless on newer).
    find_package(Threads REQUIRED)
    target_link_libraries(G-Diffuser PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    # Release tarballs carry the dependencies no distribution can be relied on to provide in
    # lib/ beside the executable (tools/package_linux.sh). $ORIGIN resolves them at load time,
    # which beats a wrapper script because the binary stays directly runnable. No effect on a
    # local build, where lib/ does not exist and the loader falls through to the system paths.
    set_target_properties(G-Diffuser PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
endif()

if(GDX_PORT_WARN_FLAGS)
    target_compile_options(G-Diffuser PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Same UB-hardening as gdiffuser_game/cxd4_rsp: the port glue reinterprets N64 RDRAM through
# host pointers and resolves host ranges by bit-pattern — all strict-aliasing-sensitive.
if(NOT MSVC)
    target_compile_options(G-Diffuser PRIVATE -fno-strict-aliasing -fwrapv)
    # Linux audio fix: the decomp's sinf/cosf were exported from .dynsym and interposed the ones
    # libSDL3's resampler imports -> silent/garbled audio. The version script localizes them so
    # libSDL3 gets glibc's. Full writeup in port/gdx_hide_libm.map.
    target_link_options(G-Diffuser PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gdx_hide_libm.map")
endif()
if(TARGET libultraship)
    target_link_libraries(G-Diffuser PRIVATE libultraship)
    target_include_directories(G-Diffuser PRIVATE
        "${CMAKE_SOURCE_DIR}/libultraship/src"
        "${CMAKE_SOURCE_DIR}/libultraship/include"
    )
endif()

# DSP unit-test harness: compiles n64_audio_hle.c UNMODIFIED and drives it black-box through
# gdx_audio_hle_run (design rationale in dsp_tests.c's header). Stand-ins for the two
# gdx_resolve_*_host_address externs live in dsp_tests.c, so n64_gfx_bridge.cpp stays out.
# gdx_dev_gates.c is linked because n64_audio_hle.c and port_log.h read the gate cache; no
# console is bound, so gates keep their env-seeded values as the _putenv-based tests expect.
add_executable(gdx_dsp_tests
    tests/dsp_tests.c
    n64_audio_hle.c
    gdx_dev_gates.c)
if(MSVC)
    target_compile_options(gdx_dsp_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_dsp_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
    target_link_libraries(gdx_dsp_tests PRIVATE m) # sin/floor: libm is separate on UNIX
endif()

# PCM capture unit test: compiles gdx_audio_capture.c UNMODIFIED and drives its public API plus
# the test-only reset seam. No game objects, no libultraship, no decomp headers.
add_executable(gdx_pcm_capture_tests
    tests/pcm_capture_tests.c
    gdx_audio_capture.c
    gdx_dev_gates.c)   # port_log.h's log/trace gates read the developer-gate cache
if(MSVC)
    target_compile_options(gdx_pcm_capture_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_pcm_capture_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# VI-scanout fallback unit test: compiles gdx_vi_convert.c UNMODIFIED and drives
# gdx_convert_rgba5551_to_rgba8888 with known pixel patterns. No game objects, no libultraship.
add_executable(gdx_vi_fallback_tests
    tests/test_vi_fallback.c
    gdx_vi_convert.c)
if(MSVC)
    target_compile_options(gdx_vi_fallback_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_vi_fallback_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Wide-Gfx packing test: compiles only the real decomp gbi.h with the game's defines, so the
# gSP*/gDP* macros under test are exactly the ones the game emits. Verifies w1 holds a full
# 64-bit host pointer and a bridge-style reader recovers it. See tests/gfx_pack_tests.c.
add_executable(gdx_gfx_pack_tests tests/gfx_pack_tests.c)
target_include_directories(gdx_gfx_pack_tests PRIVATE "${DECOMP}/include")
target_compile_definitions(gdx_gfx_pack_tests PRIVATE
    PORT=1
    GDIFFUSER_PORT=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_gfx_pack_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_gfx_pack_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Binary-DL converter test: compiles n64_gfx_convert.cpp UNMODIFIED and drives its public API
# with synthetic 8-byte N64 display lists. No game objects, no libultraship.
add_executable(gdx_gfx_convert_tests
    tests/gfx_convert_tests.cpp
    n64_gfx_convert.cpp)
target_include_directories(gdx_gfx_convert_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_gfx_convert_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_gfx_convert_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Resource smoke test: links libultraship only (no game objects), so the .o2r pipeline can be
# verified independently of the full game.
if(TARGET libultraship)
    add_executable(resource_smoketest resource_smoketest.cpp resource/ResourceFactories.cpp)
    target_include_directories(resource_smoketest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/libultraship/src"
        "${CMAKE_SOURCE_DIR}/libultraship/include"
    )
    target_link_libraries(resource_smoketest PRIVATE libultraship)
endif()

# LLE audio boot test: proves the cxd4 interpreter boots and executes the real aspMain microcode
# with the correct IMEM byte-swap, DMEM endianness, and OSTask boot contract. Skips gracefully if
# the ROM-extracted ucode blobs are absent. See docs/LLE_IMPL_STATE.md.
add_executable(gdx_rsp_boot_tests
    rsp/tests/rsp_boot_test.c
    rsp/cxd4/gdx_rsp_driver.c
    rsp/cxd4/su.c
    rsp/cxd4/vu/add.c
    rsp/cxd4/vu/divide.c
    rsp/cxd4/vu/logical.c
    rsp/cxd4/vu/multiply.c
    rsp/cxd4/vu/select.c
    rsp/cxd4/vu/vu.c)
target_include_directories(gdx_rsp_boot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rsp/cxd4")
target_compile_definitions(gdx_rsp_boot_tests PRIVATE
    SP_EXECUTE_LOG=1
    GDX_RSP_TEST_STEP_HOOK=1   # test provides its own step_SP_commands; exclude the driver's watchdog one
    _CRT_SECURE_NO_WARNINGS=1
    "RSP_BLOB_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/rsp\"")
if(MSVC)
    target_compile_options(gdx_rsp_boot_tests PRIVATE /W3)
else()
    # Vendored C89 cxd4 sources dominate this target.
    target_compile_options(gdx_rsp_boot_tests PRIVATE -w)
endif()

# EXPERIMENTAL second binary — G-Diffuser-JP (VERSION_JP profile).
# The port is US-only in three compile-time layers (binding tables, golden constants, VERSION_JP
# decomp branches), so US+JP cannot share one executable; the frozen decision
# (QUALITY_PARITY_ROADMAP §R5) is two binaries from one tree sharing one data dir. Default OFF so
# the default build is byte-unchanged. This does not compile yet: the Torch-generated JP asset
# headers do not exist, the VERSION_JP decomp sites have never been built here, the EK gen files
# are US-EK placeholders, and gen/LinkStubs.jp.c needs regenerating from a JP link log.
option(GDX_BUILD_JP "Build the EXPERIMENTAL second binary G-Diffuser-JP (VERSION_JP profile)" OFF)

if(GDX_BUILD_JP)
    message(STATUS "G-Diffuser: GDX_BUILD_JP=ON — configuring EXPERIMENTAL VERSION_JP target G-Diffuser-JP.")

    # Mirror of gdiffuser_game with the JP profile: only the defines and the generated
    # binding/stub files differ.
    add_library(gdiffuser_game_jp OBJECT ${GAME_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"
        "${DECOMP}/src/libultra/io/devmgr.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/AssetBindings.jp.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/LinkStubs.jp.c")

    if(GDX_EXPANSION_KIT)
        add_dependencies(gdiffuser_game_jp gdiffuser_ek_compile_assets)
        target_sources(gdiffuser_game_jp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/n64_leo.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkAssetBindings.c"   # US-EK bindings reused; jp/ek TODO
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkLinkStubs.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_ek_disk_overrides.c")
        target_compile_definitions(gdiffuser_game_jp PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
    endif()

    if(WIN32)
        target_sources(gdiffuser_game_jp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_win32.c")
    else()
        target_sources(gdiffuser_game_jp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_ucontext.c")
    endif()

    target_include_directories(gdiffuser_game_jp PRIVATE
        "${DECOMP}"
        "${DECOMP}/include"
        "${DECOMP}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include")   # needs include/assets/jp/rev0 (owner-run)

    # Same define set as gdiffuser_game, but the JP profile triplet.
    target_compile_definitions(gdiffuser_game_jp PRIVATE
        ${GDX_DEV_TOOLS_DEFS}
        PORT=1
        GDIFFUSER_PORT=1
        NON_MATCHING=1
        NON_EQUIVALENT=1
        AVOID_UB=1
        F3DEX_GBI_2=1
        _LANGUAGE_C=1
        VERSION_JP=1
        ASSET_VERSION=jp
        ASSET_REVISION=rev0
    )
    if(MSVC)
        target_compile_options(gdiffuser_game_jp PRIVATE /W0 /w14715 /w14716)
    else()
        target_compile_options(gdiffuser_game_jp PRIVATE -w -Werror=return-type -fcommon
            -fno-strict-aliasing -fwrapv
            -Wno-error=int-conversion
            -Wno-error=incompatible-pointer-types
            -Wno-error=implicit-function-declaration)
    endif()

    # Mirror of G-Diffuser, linking the JP game objects. The registry/widgets/events TUs are
    # REQUIRED here too: this target compiles the same gdx_menu.cpp and PORT-gated racer.c, so
    # omitting them is an unresolved external, not a missing feature.
    add_executable(G-Diffuser-JP
        main.cpp
        gdx_gui.cpp
        gdx_menu.cpp
        gdx_menu_registry.cpp
        gdx_console_log.cpp
        ui/UIWidgets.cpp
        enhancements/events/GameEvents.cpp
        enhancements/tuning/BoostDuration.cpp
        gdx_discord.cpp
        input_bridge.c
        gdx_input_script.c
        n64_gfx_bridge.cpp
        gdx_interp.cpp
        n64_gfx_convert.cpp
        gdx_vi_convert.c
        n64_audio_hle.c
        gdx_audio_lle.c
        gdx_audio_thread.cpp
        gdx_audio_capture.c
        gdx_frame_pacer.c
        shims.c
        AssetLoader.cpp
        resource/ResourceFactories.cpp
        gdx_firstboot.cpp
        gdx_extract_launch.cpp
        gdx_dump_launch.cpp
        gdx_firstboot_gui.cpp
        rom_buffer.cpp
        sram_buffer.cpp
        disk_savefile.cpp
        gdx_ghost_io.c
        gdx_ghost_window.cpp
        gdx_input_viewer.cpp
        gdx_fps_overlay.cpp
        gdx_perf.cpp
        gdx_dev_gates.c
        gdx_imgui_nav.cpp
        gdx_workshop.cpp
        gdx_audio_seq_packs.cpp
        gdx_audio_sample_packs.cpp
        gdx_audio_soundfont_packs.cpp
        gdx_lua.cpp
        gdx_lua_state.c
        gdx_model_packs.cpp
        mio0_wrap.c
        gdx_segment_source.c
        "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
        $<TARGET_OBJECTS:gdiffuser_game_jp>)

    # The exe TUs must see VERSION_JP too: they include decomp headers whose region branches
    # depend on it. GDX_ALLOW_JP_INPUTS is unconditional here regardless of the option — this
    # target IS the Japanese profile; refusing JP data would leave it nothing to load.
    target_compile_definitions(G-Diffuser-JP PRIVATE
        ${GDX_DEV_TOOLS_DEFS}
        PORT=1 GDIFFUSER_PORT=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_JP=1
        GDX_ALLOW_JP_INPUTS=1
        GDX_VERSION_STRING="${GDX_DISPLAY_VERSION}"
        ASSET_VERSION=jp ASSET_REVISION=rev0)

    if(GDX_EXPANSION_KIT)
        # EK-only symbols — see the matching US-target comment above.
        target_sources(G-Diffuser-JP PRIVATE disk_buffer.cpp gen/EkTranslatedOverrides.c
                                             gdx_ek_strings.c gen/EkTranslatedStrings.c)
        target_compile_definitions(G-Diffuser-JP PRIVATE EXPANSION_KIT=1)
    endif()

    if(WIN32)
        set_target_properties(G-Diffuser-JP PROPERTIES WIN32_EXECUTABLE TRUE)
        target_sources(G-Diffuser-JP PRIVATE gdiffuser.rc)
        if(MSVC)
            # No /DYNAMICBASE:NO — see the note on the US target above.
            target_link_options(G-Diffuser-JP PRIVATE "/MAP")
        endif()
    endif()

    target_include_directories(G-Diffuser-JP PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${DECOMP}/include"
        "${DECOMP}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0")

    target_link_libraries(G-Diffuser-JP PRIVATE cxd4_rsp)
    target_link_libraries(G-Diffuser-JP PRIVATE discord-rpc)
    target_link_libraries(G-Diffuser-JP PRIVATE gdx_lua)

    if(WIN32)
        target_link_libraries(G-Diffuser-JP PRIVATE user32 Comdlg32 Comctl32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(G-Diffuser-JP PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    if(GDX_PORT_WARN_FLAGS)
        target_compile_options(G-Diffuser-JP PRIVATE ${GDX_PORT_WARN_FLAGS})
    endif()
    if(NOT MSVC)
        target_compile_options(G-Diffuser-JP PRIVATE -fno-strict-aliasing -fwrapv)
        target_link_options(G-Diffuser-JP PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gdx_hide_libm.map")
    endif()
    if(TARGET libultraship)
        target_link_libraries(G-Diffuser-JP PRIVATE libultraship)
        target_include_directories(G-Diffuser-JP PRIVATE
            "${CMAKE_SOURCE_DIR}/libultraship/src"
            "${CMAKE_SOURCE_DIR}/libultraship/include")
    endif()

    # Deploy engine archive + extractor + recipe tree (US rev0 AND JP rev0 so one binary can
    # build either profile's archive).
    add_dependencies(G-Diffuser-JP gdiffuser_engine_o2r gdx_extract_ext)
    add_custom_command(TARGET G-Diffuser-JP POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDIFFUSER_ENGINE_O2R}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdiffuser.o2r"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDX_EXTRACT_EXE}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}"
        VERBATIM)
    add_custom_command(TARGET G-Diffuser-JP POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/us"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/jp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDX_RECIPES_SRC}/config.yml" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/config.yml"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDX_EK_SLICE_MANIFEST}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/ek_slice_manifest.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${GDX_RECIPES_SRC}/assets/yaml/us/rev0" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/us/rev0"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${GDX_RECIPES_SRC}/assets/yaml/jp/rev0" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/jp/rev0"
        VERBATIM)
endif()
