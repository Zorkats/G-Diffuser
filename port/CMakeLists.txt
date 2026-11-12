# port/ â€” game glue + decomp game sources compiled for the host. Kept standalone-capable so the
# decomp compile can iterate without rebuilding libultraship.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(gdiffuser_port LANGUAGES C CXX)
endif()

set(DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GDX_DISPLAY_VERSION "1.3.0" CACHE STRING "Version shown in the About menu")
# The release workflows pass the channel and commit; a local build stays "dev", which never
# offers updates (gdx_update.cpp).
set(GDX_BUILD_CHANNEL "dev" CACHE STRING "Update channel: stable, nightly or dev")
set(GDX_BUILD_COMMIT "" CACHE STRING "Short commit hash of this build")
# PowerShell splits an unquoted -DGDX_DISPLAY_VERSION=1.2.3 at the first period, and the cache
# then keeps "1" over the default above. Nightly versions carry a date and hash instead.
if(NOT GDX_BUILD_CHANNEL STREQUAL "nightly" AND NOT GDX_DISPLAY_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "GDX_DISPLAY_VERSION \"${GDX_DISPLAY_VERSION}\" is not an x.y.z version. "
                        "Quote the whole argument: \"-DGDX_DISPLAY_VERSION=1.2.3\".")
endif()
set(GDX_BUILD_DEFS
    GDX_VERSION_STRING="${GDX_DISPLAY_VERSION}"
    GDX_BUILD_CHANNEL="${GDX_BUILD_CHANNEL}"
    GDX_BUILD_COMMIT="${GDX_BUILD_COMMIT}")
# Test only: the release JSON URL the update check asks instead of GitHub, so the updater can be run
# end to end against a local https server. The release workflows never set it.
set(GDX_UPDATE_TEST_API_URL "" CACHE STRING "Test only: release JSON URL for the update check")
if(GDX_UPDATE_TEST_API_URL)
    message(WARNING "GDX_UPDATE_TEST_API_URL is set: this build checks ${GDX_UPDATE_TEST_API_URL} for updates")
    list(APPEND GDX_BUILD_DEFS GDX_UPDATE_TEST_API_URL="${GDX_UPDATE_TEST_API_URL}")
endif()

# Warning flags for the port's own targets under GCC/Clang; the decomp library and vendored cxd4
# stay -w (not ours to fix). No -Werror: a Linux first-configure should surface warnings, not fail.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(GDX_PORT_WARN_FLAGS -Wall -Wno-unused-parameter)
else()
    set(GDX_PORT_WARN_FLAGS "")
endif()

# Decomp game C compiled for PC. libultra/* is excluded (libultraship provides it) except gu/
# (matrix math) and os/ â€” we run the decomp's own cooperative thread scheduler and supply only
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

# Time/timers come from libultraship (os_time.cpp) â€” exclude the decomp's to avoid duplicate
# symbols. Threading/messaging/events stay decomp.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/gettime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settimer\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/timerintr\\.c$")
# Our osCreateThread (port/n64_sched.c) captures entry+arg as full 64-bit pointers; the decomp's
# truncates them into u32 context fields.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/createthread\\.c$")
# The decomp's osInitialize does real PIF/PI register I/O â€” replaced in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/initialize\\.c$")
# The decomp's osGetMemSize dereferences N64 bus addresses â€” replaced in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/getmemsize\\.c$")

# Expansion Kit: EXPANSION_KIT activates the decomp's EK hooks WITHOUT VERSION_JP â€” the
# VERSION_JP sites are JP-cart text/font differences deliberately kept on US behavior.
# EK modes are runtime-gated by disk-image presence (port/n64_leo.c).
option(GDX_EXPANSION_KIT "Compile Expansion Kit (64DD) support into the port" ON)

# GDX_DEV_TOOLS â€” master switch for the behaviour-altering (Bucket B) developer gates
# (port/gdx_dev_gates.h, docs/menu/DEVELOPER_TAB.md). When absent, every Bucket B gate is a
# hard-wired 0 (no getenv, no CVar read) and the Dev Tools page omits the section â€” safe because
# the gate table normalizes 0 == stock behaviour. ON everywhere except Release; the generator
# expression keeps one build tree correct under both single- and multi-config generators.
# GDX_FORCE_DEV_TOOLS keeps the gates in a Release soak/QA binary.
option(GDX_FORCE_DEV_TOOLS "Keep the behaviour-altering developer gates in Release builds too" OFF)
if(GDX_FORCE_DEV_TOOLS)
    set(GDX_DEV_TOOLS_DEFS "GDX_DEV_TOOLS=1")
else()
    set(GDX_DEV_TOOLS_DEFS "$<$<NOT:$<CONFIG:Release>>:GDX_DEV_TOOLS=1>")
endif()

# GDX_ALLOW_JP_INPUTS â€” whether the ORDINARY (US) binary may ingest Japanese-region game data.
# Not GDX_BUILD_JP (that builds a second VERSION_JP executable).
#
# Defaults OFF because the JP raw-ROM path plays through US-profile binding tables and produces
# loud static/blasting audio (observed, owner testing) â€” an ear-safety hazard, so release builds
# refuse JP inputs outright. Gated, not deleted; JP support is a later release.
# Covered sites: gdx_firstboot.cpp (refuses JP ROM by SHA-1 and header country code, and the
# retail JP EK disk), rom_buffer.cpp (runtime cartridge loader), disk_buffer.cpp (drops JP disk
# filenames, refuses the retail JP image by CRC-64). The 64DD IPL is deliberately NOT gated: the
# drive firmware is Japanese by existence and carries no game audio.
# The G-Diffuser-JP target always sets it â€” building that target is the opt-in.
option(GDX_ALLOW_JP_INPUTS
       "Accept Japanese-region game data (JP cartridge ROM, retail JP Expansion Kit disk). DEV ONLY â€” the JP raw-ROM path has broken audio." OFF)
if(GDX_ALLOW_JP_INPUTS)
    message(STATUS "G-Diffuser: GDX_ALLOW_JP_INPUTS=ON â€” Japanese ROM/disk inputs are ACCEPTED (experimental; JP audio is known-bad).")
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
    # save_buffer.c) â€” alternatives, never compiled together.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/save_buffer\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i11/")
    # ead_demo (64DD attract demo) has broken WIP C bodies upstream â€” exclude until the decomp
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

# libultraship already provides these libultra functions â€” exclude to avoid duplicate symbols.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/lib/aisetnextbuf\\.c$")  # osAiSetNextBuffer

# segment.c / cartridge_offsets.c: N64 ROM-segment system â€” linker segment symbols don't exist
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
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_branch.c"       # Course Edit forks: race runtime (ring 2)
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_splits_course.c" # CM4 splits: the raced course's checkpoints and mode
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_contact.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_cpu_native.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_disk.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_snapshot_native.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_snapshot_mfs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_branch_editor.c" # Course Edit forks: editor hooks and tools
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_det_layout.c"         # GDX_DET_LOG probe fields; needs the decomp Racer layout
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_net_race_bridge.c"    # O3 remote racers: writes a pose into a Racer slot
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_split_gp.c"           # MP2 split-screen Grand Prix rules
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_main_menu_grid.c"     # O2-9 main menu 3x3 card grid
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_gnet_sign.c"          # O2-9 G-NET card composer
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_gnet_sign_map.c"      # O2-9 G-NET card map (tools/gen_gnet_sign.py)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_native.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_undo.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_split_tex.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_insert_tex.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_link_tex.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_bank_tex.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_lap_digits.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_dash_pad.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_wind.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_test_drive.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_death_race.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_leo.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkAssetBindings.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkLinkStubs.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_ek_disk_overrides.c")
    # MFS_VERSION=1 and BUILD_VERSION=VERSION_J (2.0J libultra API shapes) match the decomp
    # Makefile's EK configuration; sethwinterrupt.c and the headers are gated on it.
    target_compile_definitions(gdiffuser_game PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
endif()

# Fiber backend for the scheduler (n64_sched.c -> gdx_fiber.h): Win32 fibers on Windows,
# POSIX ucontext on desktop UNIX, and a hand-written context switch on Android.
#
# Android is not a third flavour of taste: bionic ships <ucontext.h> as an alias for the
# mcontext TYPES only and implements none of getcontext/makecontext/swapcontext, so the POSIX
# backend does not link there at all. gdx_fiber_asm.c is the ucontext backend with the one
# missing primitive supplied by gdx_fiber_switch.S; everything above it is the same.
if(WIN32)
    target_sources(gdiffuser_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_win32.c")
elseif(ANDROID)
    enable_language(ASM)
    target_sources(gdiffuser_game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_asm.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_switch.S")
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
# caused real crashes: C4715/C4716 (non-void falls off the end â€” MIPS left the value in $v0,
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

if(ANDROID)
    # The NDK compiles everything with -D_FORTIFY_SOURCE=2, and Bionic's fortified bcopy/bzero
    # are __overloadable functions rather than plain declarations. That turns the decomp's habit
    # of handing them a raw N64 address -- an integer where a pointer is expected -- from the
    # warning it is under glibc into "no matching function", which no -Wno-error can reach,
    # because overload resolution failing is not a warning.
    #
    # Only the decompiled game is compiled this way. The port's own C++ keeps the hardening; it
    # is modern code that never had the habit.
    target_compile_options(gdiffuser_game PRIVATE -U_FORTIFY_SOURCE)
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

# Lua 5.4.8: vendored static lib for the scripting layer. Dormant for now â€” linked by both
# executables but no port code references it yet (the runtime core lands separately).
add_subdirectory(third_party/lua EXCLUDE_FROM_ALL)

# Ogg Vorbis, FLAC and WAV decoders for the streamed BGM replacement (gdx_music_stream.cpp).
add_subdirectory(third_party/music_decoders EXCLUDE_FROM_ALL)

# miniupnpc 2.3.3: the UPnP port mapping a G-NET room asks the router for (decision 58). Desktop only; an Android
# build compiles net/gdx_net_host_info.cpp without it, and its UPnP calls return nothing.
if(NOT ANDROID)
    add_subdirectory(third_party/miniupnpc EXCLUDE_FROM_ALL)
endif()

# The standalone updater is shipped beside desktop game executables, not in the Android APK.
if(NOT ANDROID)
    add_subdirectory(updater)
endif()

# On Android there is no executable: SDLActivity dlopens the game as a shared library, and
# "main" is the name its Java glue looks for by default (getLibraries() in MainActivity.kt).
# SDL's own <SDL_main.h> then renames our main() to SDL_main(), which is what libSDL2.so calls
# once it has a window -- so the entry point in main.cpp needs no Android branch of its own.
macro(gdx_add_main_target name)
    if(ANDROID)
        add_library(${name} SHARED ${ARGN})
        set_target_properties(${name} PROPERTIES OUTPUT_NAME "main")
    else()
        add_executable(${name} ${ARGN})
    endif()
endmacro()

gdx_add_main_target(G-Diffuser
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
    gdx_course_edit_input.c
    gdx_course_edit_help.cpp   # Course Edit hint strip and Controls Help block (F1 Enhancements > Mouse)
    gdx_course_edit_parts_panel.cpp # Course Edit panel: part widths, magnet values, track settings
    gdx_course_bounds.c        # Course Edit control-point bounds; the decomp clamp sites call in
                               # here so editor and save validator agree on the y range
    gdx_course_strings.c       # Per-course names/subtitles/BGM from the mounted archive, so a ROM
                               # hack shows its own track names instead of the retail ones
    gdx_bg_sprites.c           # Per-course skyline sprites from the mounted archive, same reason
    gdx_game_text.c            # Menu, ending and credits text from the mounted archive, same reason
    gdx_input_script.c
    gdx_det_probe.c            # GDX_DET_LOG writer; gdx_det_layout.c calls it
    gdx_input_actions.c        # 3A action layer: per-context N64 button tables, called from
                               # gdx_controller_poll before the dev overrides and mouse merges
    gdx_input_profiles.c       # 4A controller profiles: presets that write the 3A Race table
    gdx_steering_curve.c       # Per-port steering exponent, called from racer.c
    gdx_xcup_random.c          # X Cup random courses, called from course.c, cpu.c and background.c
    n64_gfx_bridge.cpp
    gdx_host_placement.c       # Registered buffers placed where no N64 address can name them
    gdx_interp.cpp
    n64_gfx_convert.cpp
    gdx_vi_convert.c
    n64_audio_hle.c            # standalone C, deliberately not part of gdiffuser_game
    gdx_audio_lle.c
    gdx_audio_mix.c            # Music/SFX volume scale, called from decomp effects.c
    gdx_music_stream.cpp       # 6A streamed BGM replacement: worker decode, ring, os.cpp mixer hook
    gdx_course_music.cpp       # 7A custom Course Edit soundtracks: music scan, store choice, stream override
    gdx_ek_text.c              # Course Edit names and number badges composed from the stock EK textures
    gdx_edit_cup.c             # Edit Cup slot names split into CRS_ENTRY.CENT and .CENL (decomp MFS calls it)
    gdx_course_state.cpp
    gdx_course_snapshot.cpp
    gdx_course_snapshot_state.cpp
    gdx_course_snapshot_io.cpp
    gdx_track_recovery.cpp
    gdx_track_recovery_runtime.cpp
    gdx_recovery_file.cpp
    gdx_recovery_metadata.cpp       # Registry phase 2: course-elements.gdxs load and flush
    gdx_course_packs.cpp       # Registry phase 2: mods/<pack>/elements.txt and the custom skybox rows
    gdx_audio_thread.cpp
    gdx_audio_capture.c
    gdx_frame_pacer.c          # default OFF: LUS's Fast3D backend already paces EndFrame() to 60fps
    shims.c
    AssetLoader.cpp
    resource/ResourceFactories.cpp
    gdx_firstboot.cpp
    gdx_user_folders.cpp
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
    gdx_foreign_track.c            # F3 foreign track normalize + checksum (pure C, shared with the Companion)
    gdx_content_pack.cpp           # .gdxp share packages: zip of a .gdxc, its sidecars and songs
    gdx_course_registry.c          # Course Edit element registry + GXE1 sidecar codec (pure C)
    gdx_course_branch_format.c     # Course forks: GdxBranchSet model, GXB1 block and GBS1 file (pure C)
    gdx_course_branch_view.c       # Course forks: the active set in plain types for global.h code
    gdx_course_minimap.c
    gdx_course_junction.c
    gdx_course_gadget_budget.c
    gdx_course_cpu.c
    gdx_course_branch_edit.c       # Course forks: editor fork set, path B view swap, undo state (pure C)
    gdx_course_settings.c          # Per-track and per-point values: GXK1 block, GKS1 file, .gdxb sidecar (pure C)
    gdx_course_settings_view.c     # The active values in plain types for global.h code
    gdx_course_meta.c              # Creator metadata: author, Edited by, staff time; GXM1, GMS1, .gdxm (pure C)
    gdx_course_meta_view.c         # The raced track's staff time CPU target in plain types for cpu.c
    gdx_course_splits.c            # CM4 staff run splits: GXM1 tag 5 codec, run recorder, pacing curve (pure C)
    gdx_staff_ghost.c              # CM3 staff ghost: the staff run's replay, GXM1 tag 7 codec (pure C)
    gdx_export_all.c               # EX export everything: which files go in the zip, by group (pure C)
    gdx_export_archive.cpp         # EX export everything: walks the install and writes the zip (libzip)
    gdx_export_page.cpp            # EX export everything: the Workshop > Tools section
    gdx_course_splits_view.c       # CM4 splits recording for racer.c and Expert and Master pacing for cpu.c
    gdx_draw_distance.c            # Draw distance and Extended draw distance scales (issue #45)
    gdx_course_sign.c              # CM5 Ed25519 track signatures over Monocypher (pure C)
    gdx_palette_crc.c              # HT palette CRC and .pal/ variant names for palette-aware HD textures (pure C)
    gdx_course_meta_sign.c         # CM5 signature rules: keep, drop or sign on export, check on import
    gdx_profile_keys.c             # CM5 signing seeds: GPK1 profile-keys.gpk codec (pure C)
    gdx_profile.c                  # Player profiles: GPF1 profile and GPS1 store codecs (pure C)
    gdx_profile_state.cpp          # The live profile store, its revision and random profile ids
    gdx_profile_page.cpp           # Online > Profile menu page
    net/gdx_net_socket.cpp         # O2 non-blocking UDP socket (Winsock / POSIX)
    net/gdx_net_channel.cpp        # O2 reliable and unreliable messages over one UDP peer
    net/gdx_net_lobby.cpp          # O2 lobby messages, LAN discovery packets and the chat filter
    net/gdx_net_room.cpp           # O3 room state machine: vote, lottery, pick, lock-in and the phase timers
    net/gdx_net_lottery.cpp        # O6.4 the Open room's lottery commitment and its reveal
    net/gdx_net_succession.cpp     # D28 who takes the room over when its host is lost
    gdx_net_machine_select.c       # O3 lock-in screen: when the stock machine screens belong to the room
    gdx_net_race_box.c             # O3 room race: the quit box every racer has (continue, return to room, leave the room)
    gdx_net_race_words.c           # O3 room race: the quit box words as stock-style CI textures (original art)
    gdx_net_room_screen.c          # O3 room screen of the main menu: pages, cursor and the texts to draw
    gdx_net_text.c                 # O3 room screens: text the two stock fonts can draw, wrapping and the timer
    gdx_net_track_choice.c         # O3 vote and pick: when the stock course select belongs to the room, and its lines
    gdx_net_room_view.cpp          # O3 room screen: the flat copy of the room session and its actions
    net/gdx_net_race.cpp           # O3 remote racers: pose buffer, the guess of where a machine is now and the dev echo
    gdx_net_race_dev.c             # O3 remote racers: the GDX_NET_PUPPET_ECHO, GDX_NET_DEV_AUTOPILOT, GDX_NET_DEV_CONTACTS and GDX_NET_DEV_TRACE developer switches
    net/gdx_net_lobby_session.cpp  # O2 lobby host, joining client and LAN room browser
    gdx_net_track_cache.cpp        # O4.3 downloaded custom tracks, verified, under online/cache/tracks
    gdx_net_own_tracks.cpp         # O4.7 the Edit Cup tracks a room can race: identity and title per slot
    gdx_net_room_course.cpp        # O4.8 a room's custom track raced from memory: course image, forks, settings
    gdx_net_track_save.cpp         # O4.9 "Save to my tracks": a room's custom track into this copy's Edit Cup
    net/gdx_net_track_pack.cpp     # O4 custom tracks: a track's identity (SHA-256 of its content bytes) and the receiver's check
    net/gdx_net_transfer.cpp       # O4.2 custom track transfer: chunks, the sender and phase gate, and the host relay
    net/gdx_net_track_flow.cpp     # O4.6 who asks, offers and streams a room's custom track, and what the room hears
    net/gdx_net_https.cpp          # O5.4 shared HTTPS transport: WinHTTP on Windows, libcurl elsewhere
    net/gdx_net_codes.cpp          # O5.2 room and friend codes: Crockford base32 with a check
    net/gdx_net_directory.cpp      # O5.5 room directory client: rules, encoders, strict reader, failover
    net/gdx_net_listing.cpp        # O5.10 a hosted room's listing: register, heartbeat, takeover
    net/gdx_net_stun.cpp           # O5.6 STUN Binding client
    net/gdx_net_portmap.cpp        # O5.7 PCP and NAT-PMP port mapping, reachability checks
    net/gdx_net_gather.cpp         # O5.8 the addresses a host lists its room under
    net/gdx_net_host_info.cpp      # O5.8 system lookups: names, local addresses, default gateway
    net/gdx_net_open_rooms.cpp     # O6.2 the Open room list and Quick race cooldown
    net/gdx_net_friends.cpp        # O7.1 friend key, join proof and friend list
    gdx_net_online.cpp             # O5.10 the directory's transport, settings and the room listing driver
    gdx_net_friend_store.cpp       # O7.4 the friend list file, through the recovery writer
    gdx_lan_lobby.cpp              # O2 lobby worker thread that owns the socket
    gdx_lan_lobby_page.cpp         # Online > LAN Lobby menu page
    gdx_menu_words.c               # Menu row labels composed at runtime from words of the stock menu textures
    gdx_save_import.c              # Emulator cartridge save to native fzerox.sav image (pure C)
    gdx_save_import_stage.cpp      # Stages that image beside the save; sram_buffer applies it next launch
    gdx_update.cpp                 # Update check, verified download, and the gdx-update handoff
    gdx_update_ui.cpp              # Launch update check, update notice, About page Updates block
    gdx_rom_courses.c              # W2 ROM course table reader; converts ROM courses (including
                                   # FZEP hack courses) into CRSD payloads for the same import chain
    gdx_mfs_image.c                # A2 read-only MFS over foreign .ndd/.ram images; feeds the
                                   # direct-payload import (gdx_content_import_begin_payload)
    gdx_hackmods.cpp           # W3 ROM-hack archive discovery, single-selection and per-hack
                               # save isolation; name helpers are unit-tested standalone
    gdx_hackmods_window.cpp    # W3 ROM Hacks picker, separate from the Workshop pack list
    gdx_content_window.cpp         # Content Library window; the F12 unified Export/Import shell
    gdx_xcup_seed.c                # M3 X Cup seed codes; hooked from Course_GenerateRandomCourse
    gdx_single_race.c              # Single Race rules for Practice; hooked from racer.c and ovl_i3
    gdx_gp_resume.c                # Save GP in progress codec and file; hooked from game.c
    gdx_palette.cpp              # F2 palette editor; mirrors Machine like gdx_ghost_io.c mirrors
                                 # the ghost structs, so it links against racer.c without decomp headers
    gdx_machine_stats.cpp        # F2b machine stat editor; mirrors Machine like gdx_palette.cpp,
                                 # applies at the same func_8008D33C hook under #ifdef PORT
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
    gdx_alt_assets.cpp
    gdx_audio_seq_packs.cpp      # stage-2 audio modding; hooks live in decomp/src/audio/*/lib/load.c
    gdx_audio_sample_packs.cpp   # stage-3 audio modding; hook lives in decomp/src/audio/disk/lib/load.c
    gdx_audio_soundfont_packs.cpp # stage-4 audio modding; hooks live in decomp/src/audio/disk/lib/load.c
    gdx_lua.cpp                  # workshop Lua script packs; the decomp C shim is gdx_lua_state.c
    gdx_lua_state.c              # includes unk_structs.h directly (exe targets carry the decomp
                                 # include dirs) so gdx_lua.cpp stays decomp-header-free
    gdx_model_packs.cpp          # F4a stock-machine model packs; hooks live in decomp/src/game/racer.c
    gdx_machine_capture.cpp
    gdx_machine_export.cpp
    gdx_machine_capture_runtime.cpp
    mio0_wrap.c
    gdx_segment_source.c       # in the exe, not gdiffuser_game: needs symbols from AssetLoader.cpp,
                                # rom_buffer.cpp and gen/AssetBindings.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
    $<TARGET_OBJECTS:gdiffuser_game>)

if(GDX_EXPANSION_KIT)
    # All four files reference EK-only symbols (gdx_ek_assets_fill, machine_create labels, EK
    # overlay .data), so they must not build in US-only configs.
    target_sources(G-Diffuser PRIVATE disk_buffer.cpp gen/EkTranslatedOverrides.c
                                      gdx_ek_strings.c gen/EkTranslatedStrings.c
                                      gdx_course_edit_history.cpp)
    # shims.c guards its US-only Leo stubs on this define.
    target_compile_definitions(G-Diffuser PRIVATE EXPANSION_KIT=1)
endif()

target_compile_definitions(G-Diffuser PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    ${GDX_JP_INPUT_DEFS}
    ${GDX_BUILD_DEFS})

if(APPLE)
    # Objective-C++ source for macOS-native UI (NSOpenPanel file picker, NSAlert dialogs).
    # The top-level project() only declares C CXX, so we must enable OBJCXX explicitly
    # before CMake will compile .mm files.
    enable_language(OBJCXX)
    target_sources(G-Diffuser PRIVATE gdx_macos_ui.mm)
endif()

if(WIN32)
    set_target_properties(G-Diffuser PROPERTIES WIN32_EXECUTABLE TRUE)
    # Application icon; Linux desktop entries carry theirs separately.
    target_sources(G-Diffuser PRIVATE gdiffuser.rc)
    # Emit a linker .map so crash RVAs and low32 data addresses in gdiffuser-run.log resolve to
    # symbols without a debugger.
    #
    # DO NOT add /DYNAMICBASE:NO. It was tried, to close the ASLR low32-collision class (a random
    # image base whose low32 lands in an N64 address window misroutes reconstructed 32-bit tokens
    # â€” see the KSEG guard in decomp/src/sys/dma.c), and the fixed base 0x140000000 was worse:
    # module low32s at 0x40000000..0x41FFFFFF corrupted the translated menu/text textures
    # build-wide and null-write-crashed Fast::Interpreter::ImportTextureRgba32 at startup
    # (GDX_WIDE_ASSET_RESOLVE=0 ruled out the resolver, leaving the base). Until every low32
    # window is audited against a chosen base, a fixed base is a bigger dice roll than ASLR.
    if(MSVC)
        target_link_options(G-Diffuser PRIVATE "/MAP")
    endif()
endif()

# â”€â”€ Desktop-only deployment + the standalone extractor â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Everything from here to the include directories below stages files NEXT TO THE EXECUTABLE, which
# is not a place that exists on Android: the APK has no writable install directory, and Gradle
# assembles the same set into the app's own external files directory instead
# (android/app/build.gradle.kts, unpacked on first run by GameAssets.kt). The gdx-extract child
# process goes with it -- see the Android extraction note further down.
if(NOT ANDROID)

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

# The post-pipeline resolves file-based presets from appDir/shaders at runtime
# (GdxResolvePostShaderPath), so the curated tree ships loose beside the exe rather than
# packed into the o2r. include/ is load-bearing: pack-root presets pull colorspace-tools.h
# and friends through relative paths, and roughly a third of the curated presets fail
# translation when include/ is missing.
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:G-Diffuser>/shaders"
    VERBATIM)

# gdx-extract, the standalone deterministic O2R extractor first boot spawns as a child process.
# Isolated ExternalProject, NOT add_subdirectory'd:
# Torch pulls its own spdlog/fmt/yaml-cpp via FetchContent, and out-of-tree prevents ODR/version
# clashes with libultraship's spdlog/fmt (docs/investigation/.../W0_TORCH_DEPENDENCIES.md).
# GDX_DETERMINISTIC=ON => byte-identical archives for identical ROM + recipes. CMP0091=NEW lets
# Torch's /MT runtime take effect so the exe needs no VC++ redist.
include(ExternalProject)

# Disable floating-point contraction (fused multiply-add) in the extractor. Clang on ARM64 (Apple
# Silicon) fuses `a + b * c` into a single FMA instruction by default, which rounds once instead of
# twice. FZX::CourseData::CalculateChecksum truncates float expressions to int, so the fused result
# drifted by a few units on 20 of 26 courses, the archive missed the golden SHA-256, and first boot
# fell back to the raw ROM on every launch (wrong course textures, garbled HUD). x86-64 GCC and MSVC
# don't contract by default, and neither did the N64's FPU, so this restores parity with the golden.
# The root CMakeLists.txt already applies -ffp-contract=off to the game, but ExternalProject builds
# don't inherit add_compile_options, and torch/CMakeLists.txt overwrites CMAKE_CXX_FLAGS, so the
# flag is injected through CMAKE_PROJECT_INCLUDE (see gdx_extract_fp.cmake).
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
        -DCMAKE_PROJECT_INCLUDE:FILEPATH=${CMAKE_CURRENT_SOURCE_DIR}/gdx_extract_fp.cmake
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
# US and PAL rev0 ship; JP is built by the separate G-Diffuser-JP target below. Recipe selection
# is by matched ROM SHA1, so only the needed tree is dereferenced at runtime.
set(GDX_RECIPES_SRC "${CMAKE_SOURCE_DIR}/decomp")
set(GDX_RECIPES_DST "$<TARGET_FILE_DIR:G-Diffuser>/decomp-recipes")
# The EK slice manifest ships beside the recipes so the runtime disk step can pass it to
# `gdx-extract disk -m`.
set(GDX_EK_SLICE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/gen/ek_slice_manifest.txt")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GDX_RECIPES_DST}/assets/yaml/us"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GDX_RECIPES_DST}/assets/yaml/pal"
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
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${GDX_RECIPES_SRC}/assets/yaml/pal/rev0"
            "${GDX_RECIPES_DST}/assets/yaml/pal/rev0"
    # Dump All data files, generated from the single source of truth at build time: audio tables
    # from decomp audio_tables.c and the EK TLUT map from the EK recipe tree, so the dump classes
    # work on machines without decomp source.
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_dump_tables_data.py"
            --audio-tables "${GDX_RECIPES_SRC}/src/audio/disk/audio_tables.c"
            --out-dir "${GDX_RECIPES_DST}"
            --ek-yaml-dir "${CMAKE_SOURCE_DIR}/fzerox-expansion-kit/assets/yaml/jp"
    VERBATIM)

else()
    # â”€â”€ Android: the extractor is linked in, not spawned from disk â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    # An APK cannot ship a second executable: native code is loaded from the app's library
    # directory as shared objects, and W^X has blocked exec() out of app-writable storage since
    # API 29. Torch is therefore compiled INTO libmain.so and reached through the entry point in
    # port/android/gdx_extract_entry.cpp.
    #
    # The launcher in port/gdx_extract_launch.cpp still forks for it. That is deliberate: the
    # fork keeps the exit code, the stdout pipe the progress parser reads, and -- the reason it
    # matters on a phone -- the peak. Extraction holds the ROM and every decoded asset at once,
    # and a child that _exit()s hands all of it straight back instead of leaving the allocator
    # sitting on the arena for the rest of the session. What it drops is the exec(), and with it
    # the need for a file anyone can run.
    #
    # USE_STANDALONE=OFF builds Torch as a static library and makes it resolve spdlog through
    # find_package, which lands on the copy libultraship already fetched -- so the ODR/version
    # clash that put the desktop extractor out-of-tree does not arise here.
    set(USE_STANDALONE OFF CACHE BOOL "" FORCE)
    set(BUILD_STORMLIB OFF CACHE BOOL "" FORCE)
    # Byte-identical archives for an identical ROM + recipes, matching the desktop extractor:
    # the version-CRC gate in main.cpp compares against the same golden.
    set(GDX_DETERMINISTIC ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/torch" "${CMAKE_BINARY_DIR}/torch" EXCLUDE_FROM_ALL)
    target_link_libraries(G-Diffuser PRIVATE torch)

    # Torch's deterministic archive contract was recorded by the desktop extractor. Two otherwise
    # legal ARM/Clang choices change the bytes of its FZX:COURSE exporter:
    #
    #   * AArch64 defaults plain `char` to unsigned, while the exporter passes filename chars to an
    #     overloaded BinaryWriter and the desktop ABI sign-extends bytes >= 0x80.
    #   * Clang contracts the checksum's multiply/add expression on ARM, producing a one-unit
    #     rounding difference from the reference calculation for many stock courses.
    #
    # The affected course_data family is currently inert, but it is part of the golden content
    # digest and silently exempting it would weaken the complete-archive gate. Make the linked
    # extractor reproduce the established format instead. Keep these options on Torch only: the
    # game has its own floating-point matching contract and must not inherit extractor flags.
    target_compile_options(torch PRIVATE -fsigned-char -ffp-contract=off)

    target_sources(G-Diffuser PRIVATE
        android/gdx_extract_entry.cpp
        android/AndroidBridge.cpp      # JNI: on-screen pad, menu state, document picker, setup
        android/gdx_android_paths.cpp) # the app's files directory, for the /proc/self/exe callers
    target_include_directories(G-Diffuser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/android")
endif()

target_include_directories(G-Diffuser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"   # for "resource/ResourceFactories.h"
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0"  # mio0.h + utils.h for mio0_wrap.c and mio0.c
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/discord-rpc/thirdparty"  # rapidjson for gdx_update.cpp
)

# Ship gdx-update beside desktop builds; Android has no install directory or helper executable.
if(NOT ANDROID)
    add_dependencies(G-Diffuser gdx-update)
    add_custom_command(TARGET G-Diffuser POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:gdx-update>"
                "$<TARGET_FILE_DIR:G-Diffuser>/gdx-update${CMAKE_EXECUTABLE_SUFFIX}"
        VERBATIM)
endif()

target_link_libraries(G-Diffuser PRIVATE cxd4_rsp)
target_link_libraries(G-Diffuser PRIVATE discord-rpc)
target_link_libraries(G-Diffuser PRIVATE gdx_lua)
target_link_libraries(G-Diffuser PRIVATE gdx_music_decoders)
target_link_libraries(G-Diffuser PRIVATE monocypher)
if(TARGET gdx_miniupnpc)
    target_link_libraries(G-Diffuser PRIVATE gdx_miniupnpc)
    target_compile_definitions(G-Diffuser PRIVATE GDX_HAVE_MINIUPNPC)
endif()

if(WIN32)
    # user32: MessageBox; Comdlg32: ROM picker; Comctl32: extraction progress marquee;
    # winhttp: the update check and download; ws2_32: the O2 LAN lobby socket.
    target_link_libraries(G-Diffuser PRIVATE user32 Comdlg32 Comctl32 winhttp ws2_32 iphlpapi)
elseif(ANDROID)
    include(FetchContent)
    # Ogg/Vorbis resolve to the copies fetched in cmake/dependencies/android.cmake; the rest are
    # NDK system libraries. No Threads:: here â€” pthreads is inside Bionic's libc, and there is no
    # separate libdl to link either.
    find_package(Ogg REQUIRED)
    find_package(Vorbis REQUIRED)
    target_link_libraries(G-Diffuser PRIVATE
        Ogg::ogg
        Vorbis::vorbis
        Vorbis::vorbisenc
        Vorbis::vorbisfile
        GLESv3
        EGL
        android    # SDL_AndroidGetActivity / the JNI attach helpers
        log)       # __android_log_print, which is where port logging surfaces on a device

    # libultraship links libzip PRIVATE, so its include directories stop there â€” but
    # O2rArchive.h includes <zip.h> and the port includes that header, so it needs them too.
    # Every other platform picks zip.h up from a system include path and never notices.
    # zipconf.h is generated, hence the binary dir.
    FetchContent_GetProperties(libzip)
    if(libzip_SOURCE_DIR)
        target_include_directories(G-Diffuser PRIVATE ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
    endif()
else()
    # pthread for the fiber backend + audio thread; dl for dladdr in n64_gfx_bridge.cpp
    # (needed on older glibc, harmless on newer).
    find_package(Threads REQUIRED)
    target_link_libraries(G-Diffuser PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(APPLE)
        # macOS: Mach VM API for the memory-probe backend (n64_gfx_bridge.cpp), AppKit for native
        # file pickers / alert dialogs, and @executable_path RPATH for bundled dylibs.
        target_link_libraries(G-Diffuser PRIVATE "-framework AppKit")
        target_link_libraries(G-Diffuser PRIVATE "-framework UniformTypeIdentifiers")
        set_target_properties(G-Diffuser PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/lib")
    else()
        # Release tarballs carry the dependencies no distribution can be relied on to provide in
        # lib/ beside the executable (tools/package_linux.sh). $ORIGIN resolves them at load time,
        # which beats a wrapper script because the binary stays directly runnable. No effect on a
        # local build, where lib/ does not exist and the loader falls through to the system paths.
        set_target_properties(G-Diffuser PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN/lib")
    endif()
endif()

if(GDX_PORT_WARN_FLAGS)
    target_compile_options(G-Diffuser PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Same UB-hardening as gdiffuser_game/cxd4_rsp: the port glue reinterprets N64 RDRAM through
# host pointers and resolves host ranges by bit-pattern â€” all strict-aliasing-sensitive.
if(NOT MSVC)
    target_compile_options(G-Diffuser PRIVATE -fno-strict-aliasing -fwrapv)
    if(APPLE)
        # macOS equivalent of the Linux version-script sinf/cosf fix: -unexported_symbol keeps the
        # decomp's N64 libm out of the dynamic export table so that SDL and system frameworks resolve
        # to the real libm. Apple ld does not support --version-script.
        target_link_options(G-Diffuser PRIVATE
            "-Wl,-unexported_symbol,_sinf"
            "-Wl,-unexported_symbol,_cosf"
            "-Wl,-unexported_symbol,___sinf"
            "-Wl,-unexported_symbol,___cosf")
    else()
        # Linux audio fix: the decomp's sinf/cosf were exported from .dynsym and interposed the ones
        # libSDL3's resampler imports -> silent/garbled audio. The version script localizes them so
        # libSDL3 gets glibc's. Full writeup in port/gdx_hide_libm.map.
        target_link_options(G-Diffuser PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gdx_hide_libm.map")
    endif()
endif()
if(TARGET libultraship)
    target_link_libraries(G-Diffuser PRIVATE libultraship)
    target_include_directories(G-Diffuser PRIVATE
        "${CMAKE_SOURCE_DIR}/libultraship/src"
        "${CMAKE_SOURCE_DIR}/libultraship/include"
    )
endif()

# â”€â”€ Unit tests (host platforms only) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Every target below is an executable meant to be RUN by whoever built it. Cross-compiled for a
# phone they would build fine and could never be executed, so Android skips them: `ctest` there is
# a host concern, and the APK has no use for them.
if(NOT ANDROID)

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

# MFS-image reader unit test: compiles gdx_mfs_image.c UNMODIFIED and drives its public API
# against synthesized .ndd/.ram fixtures (BE+LE, valid + corrupt). No game objects, no
# libultraship, no decomp headers.
add_executable(gdx_mfs_image_tests
    tests/gdx_mfs_image_tests.c
    gdx_mfs_image.c)
if(MSVC)
    target_compile_options(gdx_mfs_image_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_mfs_image_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Hack-mod name helper unit test: compiles gdx_hackmods.cpp UNMODIFIED (it declares its CVar and
# logging dependencies rather than including libultraship) and checks that no archive filename can
# escape saves/ or collide with the stock save.
add_executable(gdx_hackmods_tests
    tests/gdx_hackmods_tests.cpp
    gdx_hackmods.cpp)
target_compile_features(gdx_hackmods_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_hackmods_tests PRIVATE /W3)
    # Same definition the G-Diffuser target carries, so this harness sees the same C4996
    # posture as the real build instead of warning on strcpy/strcat the module already uses.
    target_compile_definitions(gdx_hackmods_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_hackmods_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# ROM course reader unit test: compiles gdx_rom_courses.c UNMODIFIED and drives its public API
# against synthesized ROM images (.z64/.v64/.n64, clean vs patched). No real ROM is read.
add_executable(gdx_rom_courses_tests
    tests/gdx_rom_courses_tests.c
    gdx_rom_courses.c)
if(MSVC)
    target_compile_options(gdx_rom_courses_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_rom_courses_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

add_executable(gdx_course_bounds_tests
    tests/gdx_course_bounds_tests.c
    gdx_course_bounds.c)
if(MSVC)
    target_compile_options(gdx_course_bounds_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_bounds_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Course Edit element registry and GXE1 sidecar codec: standalone pure C, no decomp headers.
add_executable(gdx_course_registry_tests
    tests/gdx_course_registry_tests.c
    gdx_course_registry.c)
if(MSVC)
    target_compile_options(gdx_course_registry_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_registry_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Course forks: GdxBranchSet validation, index shifting, GXB1 and GBS1 codecs. Standalone pure C
# (reuses the CRC-32 in gdx_course_registry.c).
add_executable(gdx_course_branch_format_tests
    tests/gdx_course_branch_format_tests.c
    gdx_course_branch_format.c
    gdx_course_branch_view.c
    gdx_course_registry.c)
if(MSVC)
    target_compile_options(gdx_course_branch_format_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_branch_format_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Per-track and per-point values: GXK1 and GKS1 codecs, the .gdxb sidecar, the plain view. Standalone pure C.
add_executable(gdx_course_settings_tests
    tests/gdx_course_settings_tests.c
    gdx_course_settings.c
    gdx_course_settings_view.c
    gdx_course_branch_format.c
    gdx_course_registry.c)
if(MSVC)
    target_compile_options(gdx_course_settings_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_settings_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Creator metadata: GXM1 block, editor save rules, CPU target, GMS1 file and .gdxm sidecar. Standalone pure C.
add_executable(gdx_course_meta_tests
    tests/gdx_course_meta_tests.c
    gdx_course_meta.c
    gdx_course_splits.c
    gdx_staff_ghost.c
    gdx_course_registry.c)
if(MSVC)
    target_compile_options(gdx_course_meta_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_meta_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Course fork editing: split, link, ring hooks, path B view swap and undo state. Standalone pure C.
add_executable(gdx_course_branch_edit_tests
    tests/gdx_course_branch_edit_tests.c
    gdx_course_branch_edit.c
    gdx_course_settings.c
    gdx_course_settings_view.c
    gdx_course_branch_format.c
    gdx_course_registry.c)
if(MSVC)
    target_compile_options(gdx_course_branch_edit_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_branch_edit_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Updater: version compare, SUMS parsing and SHA-256 in gdx_update.cpp, built without libultraship.
add_executable(gdx_update_tests
    tests/gdx_update_tests.cpp
    gdx_update.cpp
    net/gdx_net_https.cpp)
target_compile_definitions(gdx_update_tests PRIVATE
    GDX_UPDATE_TESTS_STANDALONE=1
    GDX_VERSION_STRING="1.1.0" GDX_BUILD_CHANNEL="dev" GDX_BUILD_COMMIT="")
target_include_directories(gdx_update_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/discord-rpc/thirdparty")
if(WIN32)
    target_link_libraries(gdx_update_tests PRIVATE winhttp)
else()
    target_link_libraries(gdx_update_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

# gdx-update core: path checks, payload list, ustar parser and the swap plan (no libzip or zlib).
add_executable(gdx_update_apply_tests
    tests/gdx_update_apply_tests.cpp
    updater/gdx_update_apply.cpp)
target_include_directories(gdx_update_apply_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/updater")

# Composed Expansion Kit names and number badges: standalone pure C.
add_executable(gdx_ek_text_tests
    tests/gdx_ek_text_tests.c
    gdx_ek_text.c)
if(MSVC)
    target_compile_options(gdx_ek_text_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_ek_text_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Edit Cup CENT/CENL split: standalone pure C.
add_executable(gdx_edit_cup_tests
    tests/gdx_edit_cup_tests.c
    gdx_edit_cup.c)
if(MSVC)
    target_compile_options(gdx_edit_cup_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_edit_cup_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# X Cup seed code parsing and the code queue: standalone pure C.
add_executable(gdx_xcup_seed_tests
    tests/gdx_xcup_seed_tests.c
    gdx_xcup_seed.c)
if(MSVC)
    target_compile_options(gdx_xcup_seed_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_xcup_seed_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Save GP in progress: codec and file layer, standalone pure C (reuses the CRC-32 in gdx_course_registry.c).
add_executable(gdx_gp_resume_tests
    tests/gdx_gp_resume_tests.c
    gdx_gp_resume.c
    gdx_course_registry.c)
if(MSVC)
    target_compile_options(gdx_gp_resume_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_gp_resume_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# 3A action layer: context tables, press latching and the CVar cache. controller.h (decomp) supplies
# the BTN_* names, so the decomp include dir is needed.
add_executable(gdx_input_actions_tests
    tests/gdx_input_actions_tests.c
    gdx_input_actions.c)
target_include_directories(gdx_input_actions_tests PRIVATE "${DECOMP}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gdx_input_actions_tests PRIVATE
    PORT=1
    GDIFFUSER_PORT=1
    _LANGUAGE_C=1
    VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_input_actions_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_input_actions_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# 4A controller profiles: Race-table presets over the 3A layer, so both sources are linked.
add_executable(gdx_input_profiles_tests
    tests/gdx_input_profiles_tests.c
    gdx_input_actions.c
    gdx_input_profiles.c)
target_include_directories(gdx_input_profiles_tests PRIVATE "${DECOMP}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gdx_input_profiles_tests PRIVATE
    PORT=1
    GDIFFUSER_PORT=1
    _LANGUAGE_C=1
    VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_input_profiles_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_input_profiles_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Steering response curve: standalone pure C, the test stubs the CVar read.
add_executable(gdx_steering_curve_tests
    tests/gdx_steering_curve_tests.c
    gdx_steering_curve.c)
if(MSVC)
    target_compile_options(gdx_steering_curve_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_steering_curve_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
if(UNIX)
    target_link_libraries(gdx_steering_curve_tests PRIVATE m)
endif()

# X Cup random courses: standalone pure C, the test stubs the CVar read.
add_executable(gdx_xcup_random_tests
    tests/gdx_xcup_random_tests.c
    gdx_xcup_random.c)
if(MSVC)
    target_compile_options(gdx_xcup_random_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_xcup_random_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# 6A streamed BGM: loop tags from the three decoders, the BGM key table and the loop wrap. The module
# links with a CVar stub and a stub for the decomp fade getter, so no game code is pulled in.
add_executable(gdx_music_stream_tests
    tests/gdx_music_stream_tests.cpp
    gdx_music_stream.cpp
    gdx_audio_mix.c
    gdx_dev_gates.c)   # port_log.h reads the developer-gate cache
target_include_directories(gdx_music_stream_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gdx_music_stream_tests PRIVATE
    "GDX_TEST_MUSIC_DIR=\"${CMAKE_SOURCE_DIR}/devdocs/test-assets/feature-requests/music\"")
target_link_libraries(gdx_music_stream_tests PRIVATE gdx_music_decoders)
if(MSVC)
    target_compile_options(gdx_music_stream_tests PRIVATE /W3)
    # Same definition the G-Diffuser target carries, so port_log.h's strcpy/strcat stay quiet here.
    target_compile_definitions(gdx_music_stream_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(gdx_music_stream_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(gdx_music_stream_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

add_executable(gdx_music_lifecycle_tests
    tests/gdx_music_lifecycle_tests.cpp
    gdx_dev_gates.c)
target_include_directories(gdx_music_lifecycle_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_music_lifecycle_tests PRIVATE /W3 /UNDEBUG)
    target_compile_definitions(gdx_music_lifecycle_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    target_compile_options(gdx_music_lifecycle_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
    target_link_libraries(gdx_music_lifecycle_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(gdx_course_junction_tests
    tests/gdx_course_junction_tests.c
    gdx_course_junction.c)
target_include_directories(gdx_course_junction_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_junction_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_junction_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
    target_link_libraries(gdx_course_junction_tests PRIVATE m)
endif()

add_executable(gdx_track_recovery_tests
    tests/gdx_track_recovery_tests.cpp
    gdx_track_recovery.cpp
    gdx_recovery_file.cpp
    gdx_recovery_metadata.cpp
    gdx_course_registry.c
    gdx_course_branch_format.c
    gdx_course_settings.c)
target_include_directories(gdx_track_recovery_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_track_recovery_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_track_recovery_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_recovery_native_tests
    tests/gdx_recovery_native_tests.cpp
    tests/gdx_recovery_native_fixture.c
    "${DECOMP}/src/overlays/expansion_kit/gdx_recovery.c"
    gdx_edit_cup.c)
target_include_directories(gdx_recovery_native_tests PRIVATE "${DECOMP}" "${DECOMP}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gdx_recovery_native_tests PRIVATE PORT=1 EXPANSION_KIT=1 VERSION_US=1 _LANGUAGE_C=1 F3DEX_GBI_2=1)
if(MSVC)
    target_compile_options(gdx_recovery_native_tests PRIVATE /W3 /UNDEBUG /we4013)
else()
    target_compile_options(gdx_recovery_native_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_machine_capture_tests
    tests/gdx_machine_capture_tests.cpp
    tests/gdx_machine_native_fixture.c
    gdx_machine_capture.cpp
    gdx_machine_export.cpp)
target_include_directories(gdx_machine_capture_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}/include")
target_compile_definitions(gdx_machine_capture_tests PRIVATE
    PORT=1 GDIFFUSER_PORT=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1 _CRT_SECURE_NO_WARNINGS=1)
if(MSVC)
    target_compile_options(gdx_machine_capture_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_machine_capture_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
    find_package(Threads REQUIRED)
    target_link_libraries(gdx_machine_capture_tests PRIVATE Threads::Threads)
endif()

add_executable(gdx_course_minimap_tests
    tests/gdx_course_minimap_tests.c
    gdx_course_minimap.c
    gdx_course_branch_format.c
    gdx_course_registry.c)
target_include_directories(gdx_course_minimap_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_minimap_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_minimap_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
    target_link_libraries(gdx_course_minimap_tests PRIVATE m)
endif()

add_executable(gdx_course_edit_input_tests
    tests/gdx_course_edit_input_tests.c
    gdx_course_edit_input.c)
target_include_directories(gdx_course_edit_input_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_edit_input_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_edit_input_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_single_race_tests
    tests/gdx_single_race_tests.c
    gdx_single_race.c)
target_include_directories(gdx_single_race_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_single_race_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_single_race_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_course_edit_native_tests
    tests/gdx_course_edit_native_tests.c
    gdx_course_edit_native.c
    gdx_course_edit_input.c)
target_include_directories(gdx_course_edit_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${DECOMP}"
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_edit_native_tests PRIVATE
    PORT=1 GDIFFUSER_PORT=1 EXPANSION_KIT=1
    F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1
    ASSET_VERSION=us ASSET_REVISION=rev0)
if(MSVC)
    target_compile_options(gdx_course_edit_native_tests PRIVATE /W0 /we4715 /we4716 /we4013)
else()
    target_compile_options(gdx_course_edit_native_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

add_executable(gdx_background_skyline_tests
    tests/gdx_background_skyline_tests.c)
target_include_directories(gdx_background_skyline_tests PRIVATE
    "${DECOMP}"
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_background_skyline_tests PRIVATE
    PORT=1
    GDIFFUSER_PORT=1
    EXPANSION_KIT=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_background_skyline_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_background_skyline_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Course-strings test: compiles gdx_course_strings.c UNMODIFIED and drives both halves. The
# test TU supplies the four decomp tables and the archive reader as stubs, so apply() is
# exercised end to end against synthesized payloads with no archive and no ROM in sight.
add_executable(gdx_course_strings_tests
    tests/gdx_course_strings_tests.c
    gdx_course_strings.c)
target_include_directories(gdx_course_strings_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_strings_tests PRIVATE /W3)
    target_compile_definitions(gdx_course_strings_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_strings_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Skyline-sprites test: compiles gdx_bg_sprites.c UNMODIFIED against stub table and archive reader,
# with synthesized payloads only.
add_executable(gdx_bg_sprites_tests
    tests/gdx_bg_sprites_tests.c
    gdx_bg_sprites.c)
target_include_directories(gdx_bg_sprites_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_bg_sprites_tests PRIVATE /W3)
    target_compile_definitions(gdx_bg_sprites_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_bg_sprites_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
if(NOT MSVC)
    target_link_libraries(gdx_bg_sprites_tests PRIVATE m)
endif()
# Lap-digits test: compiles gdx_lap_digits.c UNMODIFIED against a stub archive reader, with synthesized
# atlases only.
add_executable(gdx_lap_digits_tests
    tests/gdx_lap_digits_tests.c
    gdx_lap_digits.c)
target_include_directories(gdx_lap_digits_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_lap_digits_tests PRIVATE /W3)
    target_compile_definitions(gdx_lap_digits_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_lap_digits_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Dash pad test: compiles gdx_dash_pad.c UNMODIFIED against a stub archive reader, with a synthesized texture.
add_executable(gdx_dash_pad_tests
    tests/gdx_dash_pad_tests.c
    gdx_dash_pad.c)
target_include_directories(gdx_dash_pad_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_dash_pad_tests PRIVATE /W3)
    target_compile_definitions(gdx_dash_pad_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_dash_pad_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
if(NOT MSVC)
    target_link_libraries(gdx_dash_pad_tests PRIVATE m)
endif()
# LLE staging window reuse rule (issue #33), header-only.
add_executable(gdx_audio_lle_stage_tests
    tests/gdx_audio_lle_stage_tests.c)
target_include_directories(gdx_audio_lle_stage_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_audio_lle_stage_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_audio_lle_stage_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Wind test: compiles gdx_wind.c UNMODIFIED.
add_executable(gdx_wind_tests
    tests/gdx_wind_tests.c
    gdx_wind.c)
target_include_directories(gdx_wind_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_wind_tests PRIVATE /W3)
    target_compile_definitions(gdx_wind_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_wind_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
if(NOT MSVC)
    target_link_libraries(gdx_wind_tests PRIVATE m)
endif()
# Host placement test: compiles gdx_host_placement.c UNMODIFIED and allocates real placed buffers.
add_executable(gdx_host_placement_tests
    tests/gdx_host_placement_tests.c
    gdx_host_placement.c
    gdx_dev_gates.c)   # port_log.h reads the developer-gate cache
target_include_directories(gdx_host_placement_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_host_placement_tests PRIVATE /W3)
    target_compile_definitions(gdx_host_placement_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_host_placement_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Test drive test: compiles gdx_test_drive.c UNMODIFIED.
add_executable(gdx_test_drive_tests
    tests/gdx_test_drive_tests.c
    gdx_test_drive.c)
target_include_directories(gdx_test_drive_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_test_drive_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_test_drive_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Death Race test: rules, course keys and the records codec and file layer, compiled UNMODIFIED
# (reuses the CRC-32 in gdx_course_registry.c, as gdx_gp_resume_tests does).
add_executable(gdx_death_race_tests
    tests/gdx_death_race_tests.c
    gdx_death_race.c
    gdx_course_registry.c)
target_include_directories(gdx_death_race_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_death_race_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_death_race_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Split-screen Grand Prix rules test: compiles gdx_split_gp.c UNMODIFIED, no game state.
add_executable(gdx_split_gp_tests
    tests/gdx_split_gp_tests.c
    gdx_split_gp.c)
target_include_directories(gdx_split_gp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_split_gp_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_split_gp_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Main menu grid and G-NET card test: compiles the grid, the composer and the generated map UNMODIFIED, with
# synthesized sign textures only.
add_executable(gdx_main_menu_grid_tests
    tests/gdx_main_menu_grid_tests.c
    gdx_main_menu_grid.c
    gdx_gnet_sign.c
    gdx_gnet_sign_map.c)
target_include_directories(gdx_main_menu_grid_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_main_menu_grid_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_main_menu_grid_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Menu-words test: compiles gdx_menu_words.c UNMODIFIED with synthesized textures only.
add_executable(gdx_menu_words_tests
    tests/gdx_menu_words_tests.c
    gdx_menu_words.c)
target_include_directories(gdx_menu_words_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_menu_words_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_menu_words_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Draw distance test: gdx_draw_distance.c with a CVarGetInteger stub, as the EK game build sees it.
add_executable(gdx_draw_distance_tests
    tests/gdx_draw_distance_tests.c
    gdx_draw_distance.c)
target_include_directories(gdx_draw_distance_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gdx_draw_distance_tests PRIVATE EXPANSION_KIT)
if(MSVC)
    target_compile_options(gdx_draw_distance_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_draw_distance_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Palette-aware HD textures (HT): palette CRC known answers, shared with tools/gen_htc_pack.py.
add_executable(gdx_palette_crc_tests
    tests/gdx_palette_crc_tests.c
    gdx_palette_crc.c)
target_include_directories(gdx_palette_crc_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_palette_crc_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_palette_crc_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Course signing test (CM5): gdx_course_sign.c over libultraship's Monocypher, RFC 8032 vectors.
add_executable(gdx_course_sign_tests
    tests/gdx_course_sign_tests.c
    gdx_course_sign.c)
target_include_directories(gdx_course_sign_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gdx_course_sign_tests PRIVATE monocypher)
if(MSVC)
    target_compile_options(gdx_course_sign_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_sign_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# CM5 signature rules: keep, drop and sign, over the real codec and Monocypher.
add_executable(gdx_course_meta_sign_tests
    tests/gdx_course_meta_sign_tests.c
    gdx_course_meta_sign.c
    gdx_course_sign.c
    gdx_course_meta.c
    gdx_course_splits.c
    gdx_staff_ghost.c
    gdx_course_registry.c)
target_include_directories(gdx_course_meta_sign_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gdx_course_meta_sign_tests PRIVATE monocypher)
if(MSVC)
    target_compile_options(gdx_course_meta_sign_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_meta_sign_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# CM5 signing seeds: the GPK1 profile-keys.gpk codec and its small store.
add_executable(gdx_profile_keys_tests
    tests/gdx_profile_keys_tests.c
    gdx_profile_keys.c
    gdx_course_registry.c)
target_include_directories(gdx_profile_keys_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_profile_keys_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_profile_keys_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# CM3 staff ghost: the staff run's replay codec, GXM1 tag 7.
add_executable(gdx_staff_ghost_tests
    tests/gdx_staff_ghost_tests.c
    gdx_staff_ghost.c)
target_include_directories(gdx_staff_ghost_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_staff_ghost_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_staff_ghost_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# EX export everything: which files of an install go in the zip, by group, and which never leave it.
add_executable(gdx_export_all_tests
    tests/gdx_export_all_tests.c
    gdx_export_all.c)
target_include_directories(gdx_export_all_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_export_all_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_export_all_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# CM4 per-node splits: the split record codec, the run recorder and the pacing curve.
add_executable(gdx_course_splits_tests
    tests/gdx_course_splits_tests.c
    gdx_course_splits.c)
target_include_directories(gdx_course_splits_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_splits_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_course_splits_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Save-import test: compiles gdx_save_import.c UNMODIFIED with synthesized save images only.
add_executable(gdx_save_import_tests
    tests/gdx_save_import_tests.c
    gdx_save_import.c)
target_include_directories(gdx_save_import_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_save_import_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_save_import_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Staged save-import test: compiles gdx_save_import_stage.cpp and gdx_save_import.c UNMODIFIED and
# works on synthesized saves in a scratch folder under the system temp directory.
add_executable(gdx_save_import_stage_tests
    tests/gdx_save_import_stage_tests.cpp
    gdx_save_import_stage.cpp
    gdx_save_import.c)
target_include_directories(gdx_save_import_stage_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_save_import_stage_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_save_import_stage_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O2 UDP socket test: compiles net/gdx_net_socket.cpp UNMODIFIED and exchanges datagrams
# between ephemeral sockets on 127.0.0.1 only, so nothing leaves the machine.
add_executable(gdx_net_socket_tests
    tests/gdx_net_socket_tests.cpp
    net/gdx_net_socket.cpp)
target_compile_features(gdx_net_socket_tests PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(gdx_net_socket_tests PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(gdx_net_socket_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_socket_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O1 profile test: GPF1/GPS1 codecs, the store and the icon helpers. Links the course meta module so
# the profile name rule is checked against the real track author validator.
add_executable(gdx_profile_tests
    tests/gdx_profile_tests.c
    gdx_profile.c
    gdx_course_meta.c
    gdx_course_splits.c
    gdx_staff_ghost.c
    gdx_course_registry.c)
target_include_directories(gdx_profile_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_profile_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_profile_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# CM1: the live profile store (edits, revision, file restore, random ids) and the Author and Edited by
# rule it feeds in gdx_course_meta.c.
add_executable(gdx_profile_state_tests
    tests/gdx_profile_state_tests.cpp
    gdx_profile_state.cpp
    gdx_profile_keys.c
    gdx_profile.c
    gdx_course_meta.c
    gdx_course_splits.c
    gdx_staff_ghost.c
    gdx_course_registry.c)
target_include_directories(gdx_profile_state_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(gdx_profile_state_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_profile_state_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_profile_state_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O2 channel test: handshake, ordering, acks and timeouts over a simulated lossy link on a
# fake clock. No socket, so it runs anywhere.
add_executable(gdx_net_channel_tests
    tests/gdx_net_channel_tests.cpp
    net/gdx_net_channel.cpp)
target_compile_features(gdx_net_channel_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_channel_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_channel_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O3 room state machine: seats, vote, lottery, pick, lock-in and every phase timer on a fake
# clock. No network and no socket.
add_executable(gdx_net_room_tests
    tests/gdx_net_room_tests.cpp
    net/gdx_net_room.cpp
    net/gdx_net_lottery.cpp
    net/gdx_net_succession.cpp
    net/gdx_net_lobby.cpp)
target_compile_features(gdx_net_room_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_room_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_room_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O3 lock-in screen: when it opens, waits and leaves, and its timer and counter text. The room and
# the menu are four functions the test supplies, so no socket, thread or game code is linked.
add_executable(gdx_net_machine_select_tests
    tests/gdx_net_machine_select_tests.c
    gdx_net_machine_select.c
    gdx_net_text.c)
target_include_directories(gdx_net_machine_select_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_net_machine_select_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_machine_select_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O3 room race: the quit box every racer has. Its rows on a host and on a client, what opens and
# closes it, the cursor, and what each row does. The room and the race are the five functions the test supplies.
add_executable(gdx_net_race_box_tests
    tests/gdx_net_race_box_tests.c
    gdx_net_race_box.c
    gdx_net_race_words.c)
target_include_directories(gdx_net_race_box_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_net_race_box_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_race_box_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O3 room screen: every page and seat, what each button does, a room of 14 and a list of 32 rooms,
# the number fields and the text pieces. The room session is the functions the test supplies.
add_executable(gdx_net_room_screen_tests
    tests/gdx_net_room_screen_tests.c
    gdx_net_room_screen.c
    gdx_net_track_choice.c
    gdx_net_text.c)
target_include_directories(gdx_net_room_screen_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_net_room_screen_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_room_screen_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O3 vote and pick on the stock course select: when the screen is the room's, what each state of the
# room makes of it, the one ballot or pick it sends and every line it draws. The room session and the menu are
# the functions the test supplies.
add_executable(gdx_net_track_choice_tests
    tests/gdx_net_track_choice_tests.c
    gdx_net_track_choice.c
    gdx_net_text.c)
target_include_directories(gdx_net_track_choice_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_net_track_choice_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_track_choice_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O3 remote racers: the pose buffer on a simulated link (a bracketed pose, lost poses at both
# delays, hold, snap), the lap bookkeeping at the line and the one-copy echo. No network and no game code.
add_executable(gdx_net_race_tests
    tests/gdx_net_race_tests.cpp
    net/gdx_net_race.cpp)
target_compile_features(gdx_net_race_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_race_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_race_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O2 LAN lobby: wire format, chat filter, host, client and room browser on a fake network. The
# socket file is linked only for FormatAddress; no socket is opened.
add_executable(gdx_net_lobby_tests
    tests/gdx_net_lobby_tests.cpp
    net/gdx_net_lobby.cpp
    net/gdx_net_room.cpp
    net/gdx_net_lottery.cpp
    net/gdx_net_succession.cpp
    net/gdx_net_lobby_session.cpp
    net/gdx_net_friends.cpp
    net/gdx_net_channel.cpp
    net/gdx_net_socket.cpp
    net/gdx_net_track_flow.cpp net/gdx_net_transfer.cpp net/gdx_net_track_pack.cpp
    gdx_course_snapshot.cpp gdx_recovery_file.cpp gdx_course_branch_format.c gdx_course_settings.c
    gdx_course_registry.c)
target_include_directories(gdx_net_lobby_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_lobby_tests PRIVATE cxx_std_17)
target_link_libraries(gdx_net_lobby_tests PRIVATE monocypher)
if(WIN32)
    target_link_libraries(gdx_net_lobby_tests PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(gdx_net_lobby_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_lobby_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Online O2 loopback test: the channel over two real UDP sockets on 127.0.0.1.
add_executable(gdx_net_loopback_tests
    tests/gdx_net_loopback_tests.cpp
    net/gdx_net_channel.cpp
    net/gdx_net_socket.cpp)
target_compile_features(gdx_net_loopback_tests PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(gdx_net_loopback_tests PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(gdx_net_loopback_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_loopback_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# The G-NET tests, registered with CTest (enable_testing() is in the top-level CMakeLists.txt), so one run of
# `ctest -C Release -R gdx_net` proves them all. Each is a standalone program that returns 0 when every check
# passed. A new G-NET test is named gdx_net_*_tests and joins this list.
foreach(gdx_net_test IN ITEMS
        gdx_net_socket_tests
        gdx_net_channel_tests
        gdx_net_room_tests
        gdx_net_machine_select_tests
        gdx_net_race_box_tests
        gdx_net_room_screen_tests
        gdx_net_track_choice_tests
        gdx_net_race_tests
        gdx_net_lobby_tests
        gdx_net_loopback_tests
        gdx_net_track_pack_tests
        gdx_net_track_cache_tests
        gdx_net_transfer_tests
        gdx_net_track_flow_tests
        gdx_net_https_tests
        gdx_net_open_rooms_tests
        gdx_net_succession_tests
        gdx_net_lottery_tests
        gdx_net_codes_tests
        gdx_net_friends_tests
        gdx_net_friend_store_tests
        gdx_net_stun_tests
        gdx_net_portmap_tests
        gdx_net_allowlist_tests
        gdx_net_directory_tests
        gdx_net_listing_tests
        gdx_net_gather_tests
        gdx_net_host_info_tests
        gdx_net_upload_tests)
    add_test(NAME ${gdx_net_test} COMMAND ${gdx_net_test})
endforeach()
# Leftover L1, the room's lock table: every setting racer, CPU or course code reads is in one class of
# port/gdx_net_rules_audit.c. The test scans the source tree it is given for the names passed to CVarGet*
# and fails on a gameplay name the table lacks or a row the source no longer has.
add_executable(gdx_net_rules_audit_tests
    tests/gdx_net_rules_audit_tests.cpp
    gdx_net_rules_audit.c)
target_include_directories(gdx_net_rules_audit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(gdx_net_rules_audit_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_rules_audit_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_rules_audit_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
add_test(NAME gdx_net_rules_audit_tests COMMAND gdx_net_rules_audit_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
# O4.1 custom track identity: the SHA-256 of a track's content bytes, the receiver's hash-before-decode check,
# and a seeded fuzz of the snapshot decoder on network bytes.
add_executable(gdx_net_track_pack_tests
    tests/gdx_net_track_pack_tests.cpp
    net/gdx_net_track_pack.cpp
    gdx_course_snapshot.cpp gdx_recovery_file.cpp gdx_course_branch_format.c gdx_course_settings.c
    gdx_course_registry.c)
target_include_directories(gdx_net_track_pack_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_track_pack_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_track_pack_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_track_pack_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O4.3 downloaded track cache: verified content bytes under their identity, least recently used dropped first,
# in a temporary directory.
add_executable(gdx_net_track_cache_tests
    tests/gdx_net_track_cache_tests.cpp
    gdx_net_track_cache.cpp gdx_user_folders.cpp net/gdx_net_track_pack.cpp
    gdx_course_snapshot.cpp gdx_recovery_file.cpp gdx_course_branch_format.c gdx_course_settings.c
    gdx_course_registry.c)
target_include_directories(gdx_net_track_cache_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_track_cache_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_track_cache_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_track_cache_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O7.4 friend list file: a round trip, a missing file, a damaged one set aside and never overwritten, and the
# refusals, in a temporary directory.
add_executable(gdx_net_friend_store_tests
    tests/gdx_net_friend_store_tests.cpp
    gdx_net_friend_store.cpp net/gdx_net_friends.cpp gdx_recovery_file.cpp)
target_include_directories(gdx_net_friend_store_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_friend_store_tests PRIVATE cxx_std_17)
target_link_libraries(gdx_net_friend_store_tests PRIVATE monocypher)
if(MSVC)
    target_compile_options(gdx_net_friend_store_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_friend_store_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O4.2 track transfer: the five transfer messages, the sender and phase gate of decision 15, the receiver,
# the paced sender and the host relay, and how long a lobby message waits behind a lost chunk on two real
# channels (decision 6).
add_executable(gdx_net_transfer_tests
    tests/gdx_net_transfer_tests.cpp
    net/gdx_net_transfer.cpp net/gdx_net_track_pack.cpp net/gdx_net_channel.cpp
    gdx_course_snapshot.cpp gdx_recovery_file.cpp gdx_course_branch_format.c gdx_course_settings.c
    gdx_course_registry.c)
target_include_directories(gdx_net_transfer_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_transfer_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_transfer_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_transfer_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O4.6 a room's custom track: the host asks the owner, relays to the guests and tells the room who holds it,
# with the two flows wired back to back in memory and every time a nowMs argument.
add_executable(gdx_net_track_flow_tests
    tests/gdx_net_track_flow_tests.cpp
    net/gdx_net_track_flow.cpp net/gdx_net_transfer.cpp net/gdx_net_track_pack.cpp net/gdx_net_lobby.cpp
    gdx_course_snapshot.cpp gdx_recovery_file.cpp gdx_course_branch_format.c gdx_course_settings.c
    gdx_course_registry.c)
target_include_directories(gdx_net_track_flow_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_track_flow_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_track_flow_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_track_flow_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O5.4 shared HTTPS transport: Get and Post against a plain-socket server on 127.0.0.1, through the same
# WinHTTP or libcurl code the updater uses.
# O5.5 room directory client: strict JSON, the URL list and every answer, against a stub transport. Monocypher signs
# the writes as the game does, so the CONTRACT.md §9 signature vectors are checked byte for byte.
add_executable(gdx_net_directory_tests
    tests/gdx_net_directory_tests.cpp
    net/gdx_net_directory.cpp)
target_include_directories(gdx_net_directory_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_directory_tests PRIVATE cxx_std_17)
target_link_libraries(gdx_net_directory_tests PRIVATE monocypher)
if(MSVC)
    target_compile_options(gdx_net_directory_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_directory_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# O5.10 a hosted room's directory listing (register, heartbeat, takeover) against a fake directory that applies the
# contract's rules. The socket file is linked for FormatAddress and ParseAddress; no socket is opened.
add_executable(gdx_net_listing_tests
    tests/gdx_net_listing_tests.cpp
    net/gdx_net_listing.cpp
    net/gdx_net_directory.cpp
    net/gdx_net_codes.cpp
    net/gdx_net_socket.cpp)
target_include_directories(gdx_net_listing_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_listing_tests PRIVATE cxx_std_17)
if(MSVC)
    target_link_libraries(gdx_net_listing_tests PRIVATE ws2_32)
    target_compile_options(gdx_net_listing_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_listing_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# O5.6 to O5.8 and O5.11: a host's endpoints and the reachability rule from STUN, PCP or NAT-PMP, IPv6 and an
# advertised address, with the servers and the gateway played by fixed packets.
add_executable(gdx_net_gather_tests
    tests/gdx_net_gather_tests.cpp
    net/gdx_net_gather.cpp
    net/gdx_net_listing.cpp
    net/gdx_net_stun.cpp
    net/gdx_net_portmap.cpp
    net/gdx_net_directory.cpp
    net/gdx_net_codes.cpp
    net/gdx_net_socket.cpp)
target_include_directories(gdx_net_gather_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_gather_tests PRIVATE cxx_std_17)
if(MSVC)
    target_link_libraries(gdx_net_gather_tests PRIVATE ws2_32)
    target_compile_options(gdx_net_gather_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_gather_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Decision 59: which local addresses a room lists (a temporary IPv6 one first) and how the log cuts an address.
# Built without miniupnpc: its UPnP calls are the stubs, and none is made.
add_executable(gdx_net_host_info_tests
    tests/gdx_net_host_info_tests.cpp
    net/gdx_net_host_info.cpp
    net/gdx_net_socket.cpp)
target_include_directories(gdx_net_host_info_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_host_info_tests PRIVATE cxx_std_17)
if(MSVC)
    target_link_libraries(gdx_net_host_info_tests PRIVATE ws2_32 iphlpapi)
    target_compile_options(gdx_net_host_info_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_host_info_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# L2 host upload budget: the race and the track burst on a fake network, with the real lobby and transfer code.
# The socket file is linked only for FormatAddress; no socket is opened.
add_executable(gdx_net_upload_tests
    tests/gdx_net_upload_tests.cpp
    net/gdx_net_lobby.cpp
    net/gdx_net_room.cpp
    net/gdx_net_lottery.cpp
    net/gdx_net_succession.cpp
    net/gdx_net_lobby_session.cpp
    net/gdx_net_friends.cpp
    net/gdx_net_channel.cpp
    net/gdx_net_socket.cpp
    net/gdx_net_track_flow.cpp
    net/gdx_net_transfer.cpp
    net/gdx_net_track_pack.cpp
    gdx_course_snapshot.cpp
    gdx_recovery_file.cpp
    gdx_course_branch_format.c
    gdx_course_settings.c
    gdx_course_registry.c)
target_include_directories(gdx_net_upload_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_upload_tests PRIVATE cxx_std_17)
target_link_libraries(gdx_net_upload_tests PRIVATE monocypher)
if(WIN32)
    target_link_libraries(gdx_net_upload_tests PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(gdx_net_upload_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_upload_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

add_executable(gdx_net_https_tests
    tests/gdx_net_https_tests.cpp
    net/gdx_net_https.cpp)
target_include_directories(gdx_net_https_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_https_tests PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(gdx_net_https_tests PRIVATE winhttp ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(gdx_net_https_tests PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(MSVC)
    target_compile_options(gdx_net_https_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_https_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O6.2 open rooms: the list cap, the shared request cooldown and Quick race ties, on fake time.
add_executable(gdx_net_open_rooms_tests
    tests/gdx_net_open_rooms_tests.cpp
    net/gdx_net_open_rooms.cpp)
target_include_directories(gdx_net_open_rooms_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_open_rooms_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_open_rooms_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_open_rooms_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# D28.2 succession: successor choice, the split-brain step and the no-successor close, on fake time.
add_executable(gdx_net_succession_tests
    tests/gdx_net_succession_tests.cpp
    net/gdx_net_succession.cpp)
target_include_directories(gdx_net_succession_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_succession_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_succession_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_succession_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O6.4 lottery commitment: commit and reveal, the draw seed and the payload decoders.
add_executable(gdx_net_lottery_tests
    tests/gdx_net_lottery_tests.cpp
    net/gdx_net_lottery.cpp)
target_include_directories(gdx_net_lottery_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gdx_net_lottery_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_lottery_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_net_lottery_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# O5 and O7 codecs with no socket: room and friend codes, the friend key, join proof and friend list, the
# STUN binding client, NAT-PMP and PCP port mapping with the reachability rule, and the signed version
# allowlist. Each test is one module and its own test file; the crypto ones link libultraship's Monocypher.
foreach(gdx_net_codec IN ITEMS codes friends stun portmap allowlist)
    set(gdx_net_codec_sources tests/gdx_net_${gdx_net_codec}_tests.cpp net/gdx_net_${gdx_net_codec}.cpp)
    if(gdx_net_codec STREQUAL "portmap")
        list(APPEND gdx_net_codec_sources net/gdx_net_stun.cpp)
    endif()
    add_executable(gdx_net_${gdx_net_codec}_tests ${gdx_net_codec_sources})
    target_include_directories(gdx_net_${gdx_net_codec}_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                               "${CMAKE_CURRENT_SOURCE_DIR}/net")
    target_compile_features(gdx_net_${gdx_net_codec}_tests PRIVATE cxx_std_17)
    if(gdx_net_codec STREQUAL "friends" OR gdx_net_codec STREQUAL "allowlist")
        target_link_libraries(gdx_net_${gdx_net_codec}_tests PRIVATE monocypher)
    endif()
    if(MSVC)
        target_compile_options(gdx_net_${gdx_net_codec}_tests PRIVATE /W3)
    elseif(GDX_PORT_WARN_FLAGS)
        target_compile_options(gdx_net_${gdx_net_codec}_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
    endif()
endforeach()
# O5.9 owner tool: makes the release signing key and signs the version allowlist (tools/gnet-allowlist).
add_executable(gnet_allowlist_tool
    ../tools/gnet-allowlist/gnet_allowlist_tool.cpp
    net/gdx_net_allowlist.cpp)
target_include_directories(gnet_allowlist_tool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_compile_features(gnet_allowlist_tool PRIVATE cxx_std_17)
target_link_libraries(gnet_allowlist_tool PRIVATE monocypher)
if(WIN32)
    target_link_libraries(gnet_allowlist_tool PRIVATE bcrypt) # BCryptGenRandom for the key
endif()
if(MSVC)
    target_compile_options(gnet_allowlist_tool PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gnet_allowlist_tool PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Game-text test: compiles gdx_game_text.c UNMODIFIED against stub text and archive reader, with
# synthesized payloads only.
add_executable(gdx_game_text_tests
    tests/gdx_game_text_tests.c
    gdx_game_text.c)
target_include_directories(gdx_game_text_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_game_text_tests PRIVATE /W3)
    target_compile_definitions(gdx_game_text_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_game_text_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
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
    add_executable(gdx_hd_texture_tests tests/gdx_hd_texture_tests.cpp)
    target_link_libraries(gdx_hd_texture_tests PRIVATE libultraship)
    target_compile_features(gdx_hd_texture_tests PRIVATE cxx_std_20)
    add_executable(gdx_alt_assets_tests tests/gdx_alt_assets_tests.cpp gdx_alt_assets.cpp)
    target_link_libraries(gdx_alt_assets_tests PRIVATE libultraship)
    target_compile_features(gdx_alt_assets_tests PRIVATE cxx_std_20)
    add_executable(gdx_content_pack_tests tests/gdx_content_pack_tests.cpp gdx_content_pack.cpp)
    target_link_libraries(gdx_content_pack_tests PRIVATE libultraship)
    target_compile_features(gdx_content_pack_tests PRIVATE cxx_std_20)
    # EX export everything: walks a made-up install and checks the zip it writes.
    add_executable(gdx_export_archive_tests tests/gdx_export_archive_tests.cpp gdx_export_archive.cpp
        gdx_export_all.c)
    target_link_libraries(gdx_export_archive_tests PRIVATE libultraship)
    target_compile_features(gdx_export_archive_tests PRIVATE cxx_std_20)
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

endif() # NOT ANDROID (unit tests)

# EXPERIMENTAL second binary â€” G-Diffuser-JP (VERSION_JP profile).
# The port is US-only in three compile-time layers (binding tables, golden constants, VERSION_JP
# decomp branches), so US+JP cannot share one executable; the frozen decision
# (QUALITY_PARITY_ROADMAP Â§R5) is two binaries from one tree sharing one data dir. Default OFF so
# the default build is byte-unchanged. This does not compile yet: the Torch-generated JP asset
# headers do not exist, the VERSION_JP decomp sites have never been built here, the EK gen files
# are US-EK placeholders, and gen/LinkStubs.jp.c needs regenerating from a JP link log.
option(GDX_BUILD_JP "Build the EXPERIMENTAL second binary G-Diffuser-JP (VERSION_JP profile)" OFF)

# NOT ANDROID: the second binary is a desktop experiment, and its deployment steps stage the
# extractor and recipes next to an executable that does not exist there.
if(GDX_BUILD_JP AND NOT ANDROID)
    message(STATUS "G-Diffuser: GDX_BUILD_JP=ON â€” configuring EXPERIMENTAL VERSION_JP target G-Diffuser-JP.")

    # Mirror of gdiffuser_game with the JP profile: only the defines and the generated
    # binding/stub files differ.
    add_library(gdiffuser_game_jp OBJECT ${GAME_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_branch.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_splits_course.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_contact.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_cpu_native.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_disk.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_snapshot_native.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_snapshot_mfs.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_branch_editor.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_det_layout.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_net_race_bridge.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_split_gp.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_main_menu_grid.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_gnet_sign.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_gnet_sign_map.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"
        "${DECOMP}/src/libultra/io/devmgr.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/AssetBindings.jp.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/LinkStubs.jp.c")

    if(GDX_EXPANSION_KIT)
        add_dependencies(gdiffuser_game_jp gdiffuser_ek_compile_assets)
        target_sources(gdiffuser_game_jp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_native.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_undo.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_split_tex.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_insert_tex.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_link_tex.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_course_edit_bank_tex.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_lap_digits.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_dash_pad.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_wind.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_test_drive.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_death_race.c"
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
        gdx_det_probe.c        # GDX_DET_LOG writer; gdx_det_layout.c calls it
        gdx_input_actions.c    # 3A action layer; input_bridge.c calls it, so this target links it too
        gdx_input_profiles.c   # 4A controller profiles; the Actions page in gdx_menu_registry.cpp calls it
        gdx_steering_curve.c   # racer.c calls it, and the Controls page in gdx_menu_registry.cpp
        gdx_xcup_random.c      # course.c, cpu.c and background.c call it, and the Gameplay page
        gdx_course_strings.c   # decomp game.c calls gdx_course_strings_apply() under PORT, so this
                               # is a link dependency and not an optional feature
        gdx_bg_sprites.c       # same for gdx_bg_sprites_apply()
        gdx_game_text.c        # same for gdx_game_text_apply()
        n64_gfx_bridge.cpp
        gdx_host_placement.c
        gdx_interp.cpp
        n64_gfx_convert.cpp
        gdx_vi_convert.c
        n64_audio_hle.c
        gdx_audio_lle.c
        gdx_audio_mix.c
        gdx_music_stream.cpp
        gdx_course_music.cpp
        gdx_ek_text.c
        gdx_edit_cup.c
        gdx_course_state.cpp
    gdx_course_snapshot.cpp
    gdx_course_snapshot_state.cpp
    gdx_course_snapshot_io.cpp
    gdx_track_recovery.cpp
    gdx_track_recovery_runtime.cpp
    gdx_recovery_file.cpp
    gdx_recovery_metadata.cpp
        gdx_course_packs.cpp
        gdx_audio_thread.cpp
        gdx_audio_capture.c
        gdx_frame_pacer.c
        shims.c
        AssetLoader.cpp
        resource/ResourceFactories.cpp
        gdx_firstboot.cpp
        gdx_user_folders.cpp
        gdx_extract_launch.cpp
        gdx_dump_launch.cpp
        gdx_firstboot_gui.cpp
        rom_buffer.cpp
        sram_buffer.cpp
        disk_savefile.cpp
        gdx_ghost_io.c
        gdx_ghost_window.cpp
        # Port sources the main G-Diffuser target also builds; this target was missing them.
        gdx_course_edit_mouse.cpp
        gdx_course_edit_input.c
        gdx_course_edit_help.cpp
        gdx_course_edit_parts_panel.cpp
        gdx_course_bounds.c
        gdx_content_io.c
        gdx_content_import.c
        gdx_foreign_track.c
        gdx_content_pack.cpp
        gdx_course_registry.c
        gdx_course_branch_format.c
        gdx_course_branch_view.c
        gdx_course_minimap.c
        gdx_course_junction.c
        gdx_course_gadget_budget.c
        gdx_course_cpu.c
        gdx_course_branch_edit.c
        gdx_course_settings.c
        gdx_course_settings_view.c
        gdx_course_meta.c
        gdx_course_meta_view.c
        gdx_course_splits.c
        gdx_staff_ghost.c
        gdx_export_all.c
        gdx_export_archive.cpp
        gdx_export_page.cpp
        gdx_course_splits_view.c
        gdx_draw_distance.c
        gdx_course_sign.c
        gdx_palette_crc.c
        gdx_course_meta_sign.c
        gdx_profile_keys.c
        gdx_profile.c
        gdx_profile_state.cpp
        gdx_profile_page.cpp
        net/gdx_net_socket.cpp
        net/gdx_net_channel.cpp
        net/gdx_net_lobby.cpp
        net/gdx_net_room.cpp
        net/gdx_net_lottery.cpp
        net/gdx_net_succession.cpp
        net/gdx_net_track_flow.cpp
        net/gdx_net_transfer.cpp
        net/gdx_net_track_pack.cpp
        net/gdx_net_codes.cpp
        net/gdx_net_directory.cpp
        net/gdx_net_friends.cpp
        net/gdx_net_gather.cpp
        net/gdx_net_host_info.cpp
        net/gdx_net_listing.cpp
        net/gdx_net_open_rooms.cpp
        net/gdx_net_portmap.cpp
        net/gdx_net_stun.cpp
        gdx_net_online.cpp
        gdx_net_friend_store.cpp
        gdx_net_track_cache.cpp
        gdx_net_machine_select.c
        gdx_net_race_box.c
        gdx_net_race_words.c
        gdx_net_room_screen.c
        gdx_net_text.c
        gdx_net_track_choice.c
        gdx_net_room_view.cpp
        gdx_net_own_tracks.cpp
        gdx_net_room_course.cpp
        gdx_net_track_save.cpp
        net/gdx_net_race.cpp
        gdx_net_race_dev.c
        net/gdx_net_lobby_session.cpp
        gdx_lan_lobby.cpp
        gdx_lan_lobby_page.cpp
        gdx_menu_words.c
        gdx_save_import.c
        gdx_save_import_stage.cpp
        gdx_update.cpp
        net/gdx_net_https.cpp
        gdx_update_ui.cpp
        gdx_rom_courses.c
        gdx_mfs_image.c
        gdx_hackmods.cpp
        gdx_hackmods_window.cpp
        gdx_content_window.cpp
        gdx_xcup_seed.c
        gdx_single_race.c
        gdx_gp_resume.c
        gdx_palette.cpp
        gdx_machine_stats.cpp
        gdx_custom_grid.c
        gdx_achievements.cpp
        gdx_input_viewer.cpp
        gdx_fps_overlay.cpp
        gdx_perf.cpp
        gdx_dev_gates.c
        gdx_imgui_nav.cpp
        gdx_workshop.cpp
        gdx_alt_assets.cpp
        gdx_audio_seq_packs.cpp
        gdx_audio_sample_packs.cpp
        gdx_audio_soundfont_packs.cpp
        gdx_lua.cpp
        gdx_lua_state.c
        gdx_model_packs.cpp
        gdx_machine_capture.cpp
        gdx_machine_export.cpp
        gdx_machine_capture_runtime.cpp
        mio0_wrap.c
        gdx_segment_source.c
        "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
        $<TARGET_OBJECTS:gdiffuser_game_jp>)

    # The exe TUs must see VERSION_JP too: they include decomp headers whose region branches
    # depend on it. GDX_ALLOW_JP_INPUTS is unconditional here regardless of the option â€” this
    # target IS the Japanese profile; refusing JP data would leave it nothing to load.
    target_compile_definitions(G-Diffuser-JP PRIVATE
        ${GDX_DEV_TOOLS_DEFS}
        PORT=1 GDIFFUSER_PORT=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_JP=1
        GDX_ALLOW_JP_INPUTS=1
        ${GDX_BUILD_DEFS}
        ASSET_VERSION=jp ASSET_REVISION=rev0)

    if(GDX_EXPANSION_KIT)
        # EK-only symbols â€” see the matching US-target comment above.
        target_sources(G-Diffuser-JP PRIVATE disk_buffer.cpp gen/EkTranslatedOverrides.c
                                             gdx_ek_strings.c gen/EkTranslatedStrings.c
                                             gdx_course_edit_history.cpp)
        target_compile_definitions(G-Diffuser-JP PRIVATE EXPANSION_KIT=1)
    endif()

    if(APPLE)
        target_sources(G-Diffuser-JP PRIVATE gdx_macos_ui.mm)
    endif()

    if(WIN32)
        set_target_properties(G-Diffuser-JP PROPERTIES WIN32_EXECUTABLE TRUE)
        target_sources(G-Diffuser-JP PRIVATE gdiffuser.rc)
        if(MSVC)
            # No /DYNAMICBASE:NO â€” see the note on the US target above.
            target_link_options(G-Diffuser-JP PRIVATE "/MAP")
        endif()
    endif()

    target_include_directories(G-Diffuser-JP PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${DECOMP}/include"
        "${DECOMP}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/discord-rpc/thirdparty")

    target_link_libraries(G-Diffuser-JP PRIVATE cxd4_rsp)
    target_link_libraries(G-Diffuser-JP PRIVATE discord-rpc)
    target_link_libraries(G-Diffuser-JP PRIVATE gdx_lua)
    target_link_libraries(G-Diffuser-JP PRIVATE gdx_music_decoders)
    target_link_libraries(G-Diffuser-JP PRIVATE monocypher)
    target_link_libraries(G-Diffuser-JP PRIVATE gdx_miniupnpc)
    target_compile_definitions(G-Diffuser-JP PRIVATE GDX_HAVE_MINIUPNPC)

    if(WIN32)
        target_link_libraries(G-Diffuser-JP PRIVATE user32 Comdlg32 Comctl32 winhttp ws2_32 iphlpapi)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(G-Diffuser-JP PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        if(APPLE)
            target_link_libraries(G-Diffuser-JP PRIVATE "-framework AppKit")
            target_link_libraries(G-Diffuser-JP PRIVATE "-framework UniformTypeIdentifiers")
            set_target_properties(G-Diffuser-JP PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "@executable_path/lib")
        endif()
    endif()

    if(GDX_PORT_WARN_FLAGS)
        target_compile_options(G-Diffuser-JP PRIVATE ${GDX_PORT_WARN_FLAGS})
    endif()
    if(NOT MSVC)
        target_compile_options(G-Diffuser-JP PRIVATE -fno-strict-aliasing -fwrapv)
        if(APPLE)
            target_link_options(G-Diffuser-JP PRIVATE
                "-Wl,-unexported_symbol,_sinf"
                "-Wl,-unexported_symbol,_cosf"
                "-Wl,-unexported_symbol,___sinf"
                "-Wl,-unexported_symbol,___cosf")
        else()
            target_link_options(G-Diffuser-JP PRIVATE
                "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gdx_hide_libm.map")
        endif()
    endif()
    if(TARGET libultraship)
        target_link_libraries(G-Diffuser-JP PRIVATE libultraship)
        target_include_directories(G-Diffuser-JP PRIVATE
            "${CMAKE_SOURCE_DIR}/libultraship/src"
            "${CMAKE_SOURCE_DIR}/libultraship/include")
    endif()

    # Deploy engine archive + extractor + recipe tree (US rev0 AND JP rev0 so one binary can
    # build either profile's archive).
    add_dependencies(G-Diffuser-JP gdiffuser_engine_o2r gdx_extract_ext gdx-update)
    add_custom_command(TARGET G-Diffuser-JP POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDIFFUSER_ENGINE_O2R}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdiffuser.o2r"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDX_EXTRACT_EXE}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:gdx-update>" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdx-update${CMAKE_EXECUTABLE_SUFFIX}"
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

if(NOT ANDROID)

add_executable(gdx_user_folders_tests
    tests/gdx_user_folders_tests.cpp
    gdx_user_folders.cpp)
target_include_directories(gdx_user_folders_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(gdx_user_folders_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_user_folders_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_user_folders_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()
add_executable(gdx_course_gadget_budget_tests tests/gdx_course_gadget_budget_tests.c gdx_course_gadget_budget.c)
target_include_directories(gdx_course_gadget_budget_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_gadget_budget_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_gadget_budget_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_course_gadgets_native_tests
    tests/gdx_course_gadgets_native_tests.c
    "${DECOMP}/src/game/course_gadgets.c"
    gdx_course_gadget_budget.c)
target_include_directories(gdx_course_gadgets_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include"
    "${DECOMP}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_gadgets_native_tests PRIVATE
    GDX_GADGET_GENERATION_TEST=1 PORT=1 GDIFFUSER_PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1
    ASSET_VERSION=us ASSET_REVISION=rev0)
if(MSVC)
    target_compile_options(gdx_course_gadgets_native_tests PRIVATE /W0 /we4715 /we4716 /we4013 /UNDEBUG)
else()
    target_compile_options(gdx_course_gadgets_native_tests PRIVATE -UNDEBUG)
    target_link_libraries(gdx_course_gadgets_native_tests PRIVATE m)
endif()

add_executable(gdx_course_cpu_tests tests/gdx_course_cpu_tests.c gdx_course_cpu.c)
if(MSVC)
    target_compile_options(gdx_course_cpu_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_cpu_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
    target_link_libraries(gdx_course_cpu_tests PRIVATE m)
endif()

add_executable(gdx_course_contact_tests tests/gdx_course_contact_tests.c
    gdx_course_contact.c "${DECOMP}/src/game/course.c")
target_include_directories(gdx_course_contact_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include"
    "${DECOMP}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_contact_tests PRIVATE
    GDX_COURSE_CONTACT_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_contact_tests PRIVATE /W0 /we4715 /we4716 /we4013 /UNDEBUG)
else()
    target_compile_options(gdx_course_contact_tests PRIVATE -UNDEBUG)
    target_link_libraries(gdx_course_contact_tests PRIVATE m)
endif()

add_executable(gdx_course_cpu_native_tests tests/gdx_course_cpu_native_tests.c
    gdx_course_cpu_native.c
    gdx_course_cpu.c)
target_include_directories(gdx_course_cpu_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include"
    "${DECOMP}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_cpu_native_tests PRIVATE
    PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_cpu_native_tests PRIVATE /W0 /we4715 /we4716 /we4013 /UNDEBUG)
else()
    target_compile_options(gdx_course_cpu_native_tests PRIVATE -UNDEBUG)
    target_link_libraries(gdx_course_cpu_native_tests PRIVATE m)
endif()

add_executable(gdx_course_disk_tests tests/gdx_course_disk_tests.c gdx_course_disk.c
    gdx_course_snapshot_native.c
    "${DECOMP}/src/game/course.c")
target_include_directories(gdx_course_disk_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include"
    "${DECOMP}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_disk_tests PRIVATE
    GDX_COURSE_CHECKSUM_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_disk_tests PRIVATE /W0 /we4715 /we4716 /we4013 /UNDEBUG)
else()
    target_compile_options(gdx_course_disk_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_content_validation_tests tests/gdx_content_validation_tests.c gdx_foreign_track.c)
target_include_directories(gdx_content_validation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_content_validation_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(gdx_content_validation_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_foreign_track_tests tests/gdx_foreign_track_tests.c gdx_foreign_track.c)
if(MSVC)
    target_compile_options(gdx_foreign_track_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_foreign_track_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_foreign_import_tests tests/gdx_foreign_import_tests.c gdx_foreign_track.c)
target_include_directories(gdx_foreign_import_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_foreign_import_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_foreign_import_tests PRIVATE -UNDEBUG)
endif()


add_executable(gdx_course_save_wait_tests tests/gdx_course_save_wait_tests.c)
target_include_directories(gdx_course_save_wait_tests PRIVATE
    "${DECOMP}" "${DECOMP}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_save_wait_tests PRIVATE
    PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_save_wait_tests PRIVATE /W3 /UNDEBUG /we4013)
else()
    target_compile_options(gdx_course_save_wait_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_course_snapshot_tests tests/gdx_course_snapshot_tests.cpp gdx_recovery_file.cpp
    gdx_course_snapshot.cpp gdx_course_branch_format.c gdx_course_settings.c gdx_course_registry.c)
target_include_directories(gdx_course_snapshot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_snapshot_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_snapshot_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
endif()

add_executable(gdx_course_snapshot_state_tests
    tests/gdx_course_snapshot_state_tests.cpp tests/gdx_course_snapshot_fixture.c
    gdx_course_snapshot_state.cpp gdx_course_snapshot.cpp gdx_course_snapshot_native.c gdx_course_disk.c
    gdx_recovery_metadata.cpp gdx_course_branch_format.c gdx_course_registry.c gdx_course_settings.c
    gdx_recovery_file.cpp "${DECOMP}/src/game/course.c")
target_include_directories(gdx_course_snapshot_state_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_snapshot_state_tests PRIVATE
    GDX_SNAPSHOT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
    GDX_COURSE_CHECKSUM_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_snapshot_state_tests PRIVATE /W3 /UNDEBUG /we4013)
else()
    target_compile_options(gdx_course_snapshot_state_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_course_snapshot_io_tests
    tests/gdx_course_snapshot_io_tests.cpp tests/gdx_course_snapshot_fixture.c
    gdx_course_snapshot_io.cpp gdx_course_snapshot_state.cpp gdx_course_snapshot.cpp gdx_course_snapshot_native.c gdx_course_disk.c
    gdx_recovery_metadata.cpp gdx_course_branch_format.c gdx_course_registry.c gdx_course_settings.c
    gdx_recovery_file.cpp "${DECOMP}/src/game/course.c")
target_include_directories(gdx_course_snapshot_io_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_snapshot_io_tests PRIVATE
    GDX_COURSE_CHECKSUM_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_snapshot_io_tests PRIVATE /W3 /UNDEBUG /we4013)
else()
    target_compile_options(gdx_course_snapshot_io_tests PRIVATE -UNDEBUG)
endif()

# Online O4.8: a room's custom track raced from memory. A real track goes through capture, content and Verify; the
# lobby, the extended height switch and the boot are stubs in the test, and the stores must stay empty.
add_executable(gdx_net_room_course_tests
    tests/gdx_net_room_course_tests.cpp tests/gdx_course_snapshot_fixture.c
    gdx_net_room_course.cpp net/gdx_net_track_pack.cpp
    gdx_course_snapshot_state.cpp gdx_course_snapshot.cpp gdx_course_snapshot_native.c gdx_course_disk.c
    gdx_recovery_metadata.cpp gdx_course_branch_format.c gdx_course_registry.c gdx_course_settings.c
    gdx_recovery_file.cpp "${DECOMP}/src/game/course.c")
target_include_directories(gdx_net_room_course_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_net_room_course_tests PRIVATE
    GDX_COURSE_CHECKSUM_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
target_compile_features(gdx_net_room_course_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_room_course_tests PRIVATE /W3 /we4013)
endif()
add_test(NAME gdx_net_room_course_tests COMMAND gdx_net_room_course_tests)

# Online O4.9: "Save to my tracks" on the results page. A real track goes through the hash check; the lobby's copy,
# the content import and the Edit Cup are stubs in the test, which finish an install at once or leave it pending.
add_executable(gdx_net_track_save_tests
    tests/gdx_net_track_save_tests.cpp tests/gdx_course_snapshot_fixture.c
    gdx_net_track_save.cpp gdx_net_text.c net/gdx_net_track_pack.cpp
    gdx_course_snapshot_state.cpp gdx_course_snapshot.cpp gdx_course_snapshot_native.c gdx_course_disk.c
    gdx_recovery_metadata.cpp gdx_course_branch_format.c gdx_course_registry.c gdx_course_settings.c
    gdx_recovery_file.cpp "${DECOMP}/src/game/course.c")
target_include_directories(gdx_net_track_save_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/net" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_net_track_save_tests PRIVATE
    GDX_COURSE_CHECKSUM_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
target_compile_features(gdx_net_track_save_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(gdx_net_track_save_tests PRIVATE /W3 /we4013)
endif()
add_test(NAME gdx_net_track_save_tests COMMAND gdx_net_track_save_tests)

add_executable(gdx_course_snapshot_mfs_tests
    tests/gdx_course_snapshot_io_tests.cpp
    tests/gdx_snapshot_recovery_fixture.cpp gdx_track_recovery_runtime.cpp gdx_track_recovery.cpp gdx_edit_cup.c
    tests/gdx_course_snapshot_mfs_fixture.c gdx_course_snapshot_mfs.c tests/gdx_course_snapshot_fixture.c
    gdx_course_snapshot_io.cpp gdx_course_snapshot_state.cpp gdx_course_snapshot.cpp gdx_course_snapshot_native.c gdx_course_disk.c
    gdx_recovery_metadata.cpp gdx_course_branch_format.c gdx_course_registry.c gdx_course_settings.c
    gdx_recovery_file.cpp "${DECOMP}/src/game/course.c")
target_include_directories(gdx_course_snapshot_mfs_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_snapshot_mfs_tests PRIVATE
    GDX_SNAPSHOT_MFS_TEST=1 GDX_COURSE_CHECKSUM_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
target_link_libraries(gdx_course_snapshot_mfs_tests PRIVATE ImGui)
if(MSVC)
    target_compile_options(gdx_course_snapshot_mfs_tests PRIVATE /W3 /UNDEBUG /we4013)
else()
    target_compile_options(gdx_course_snapshot_mfs_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_course_chunk_tests tests/gdx_course_chunk_tests.c "${DECOMP}/src/game/course.c")
target_include_directories(gdx_course_chunk_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_chunk_tests PRIVATE
    GDX_COURSE_CONTACT_TEST=1 PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_chunk_tests PRIVATE /W0 /we4715 /we4716 /we4013 /UNDEBUG)
else()
    target_compile_options(gdx_course_chunk_tests PRIVATE -UNDEBUG)
    target_link_libraries(gdx_course_chunk_tests PRIVATE m)
endif()

add_executable(gdx_course_edit_undo_tests tests/gdx_course_edit_undo_tests.c
    gdx_course_edit_undo.c gdx_course_branch_edit.c gdx_course_branch_format.c
    gdx_course_registry.c gdx_course_settings.c gdx_course_settings_view.c)
target_include_directories(gdx_course_edit_undo_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${DECOMP}" "${DECOMP}/include" "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(gdx_course_edit_undo_tests PRIVATE
    PORT=1 EXPANSION_KIT=1 AVOID_UB=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_course_edit_undo_tests PRIVATE /W3 /UNDEBUG /we4013)
else()
    target_compile_options(gdx_course_edit_undo_tests PRIVATE -UNDEBUG)
    target_link_libraries(gdx_course_edit_undo_tests PRIVATE m)
endif()

add_executable(gdx_content_import_disk_tests tests/gdx_content_import_disk_tests.c)
target_include_directories(gdx_content_import_disk_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_content_import_disk_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_content_import_disk_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_course_branch_save_tests tests/gdx_course_branch_save_tests.c)
target_include_directories(gdx_course_branch_save_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_course_branch_save_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_branch_save_tests PRIVATE -UNDEBUG)
endif()

add_executable(gdx_music_folders_tests
    tests/gdx_music_folders_tests.cpp
    gdx_course_registry.c
    gdx_audio_mix.c
    gdx_dev_gates.c)
target_include_directories(gdx_music_folders_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gdx_music_folders_tests PRIVATE gdx_music_decoders)
if(MSVC)
    target_compile_options(gdx_music_folders_tests PRIVATE /W3 /UNDEBUG)
    target_compile_definitions(gdx_music_folders_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    target_compile_options(gdx_music_folders_tests PRIVATE ${GDX_PORT_WARN_FLAGS} -UNDEBUG)
    target_link_libraries(gdx_music_folders_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(gdx_course_editor_layout_tests tests/gdx_course_editor_layout_tests.c)
target_include_directories(gdx_course_editor_layout_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gdx_course_editor_layout_tests PRIVATE PORT=1)
if(MSVC)
    target_compile_options(gdx_course_editor_layout_tests PRIVATE /W3 /UNDEBUG)
else()
    target_compile_options(gdx_course_editor_layout_tests PRIVATE -UNDEBUG)
endif()

endif() # NOT ANDROID (unit tests)
