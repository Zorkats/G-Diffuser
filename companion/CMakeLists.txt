# G-Diffuser Companion: a standalone ImGui and SDL2 tool. It links only ImGui, stb_image (theme
# backdrops), SDL2 and OpenGL, never the game. Packer work runs in a frozen worker (tools/build_companion_worker.py), which the custom
# target below copies next to the executable when it exists.
# Copyright (c) Zorkats

if(WIN32)
    find_package(SDL2 CONFIG REQUIRED)
else()
    find_package(SDL2 REQUIRED)
endif()
find_package(OpenGL REQUIRED)

# Codec only: no SDL or ImGui, so the headless protocol test links it alone.
add_library(gdx_companion_codec STATIC
    gdx_companion_json.cpp
    gdx_companion_log.cpp
    gdx_companion_protocol.cpp
)
target_include_directories(gdx_companion_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(companion_protocol_tests tests/companion_protocol_tests.cpp)
target_link_libraries(companion_protocol_tests PRIVATE gdx_companion_codec)

# The game's own pure C readers, shared rather than copied: the MFS disk image reader and the foreign
# track normalization (F3). Neither touches game state.
add_library(gdx_companion_game_c STATIC
    ${CMAKE_SOURCE_DIR}/port/gdx_mfs_image.c
    ${CMAKE_SOURCE_DIR}/port/gdx_foreign_track.c
)
target_include_directories(gdx_companion_game_c PUBLIC ${CMAKE_SOURCE_DIR}/port)

# Host logic: child process, worker session, paths, the installation check, packer forms, tracks, music,
# loop editing, loudness and project files. No SDL or ImGui.
add_library(gdx_companion_host STATIC
    gdx_companion_install.cpp
    gdx_companion_jobs.cpp
    gdx_companion_loop.cpp
    gdx_companion_loudness.cpp
    gdx_companion_music.cpp
    gdx_companion_packers.cpp
    gdx_companion_paths.cpp
    gdx_companion_process.cpp
    gdx_companion_project.cpp
    gdx_companion_themes.cpp
    gdx_companion_tracks.cpp
    gdx_companion_worker.cpp
)
# gdx_music_decoders is defined by port/, added after this folder; CMake resolves it at generate time.
target_link_libraries(gdx_companion_host PUBLIC gdx_companion_codec gdx_companion_game_c gdx_music_decoders)

add_executable(companion_packers_tests tests/companion_packers_tests.cpp)
target_link_libraries(companion_packers_tests PRIVATE gdx_companion_host)

add_executable(companion_tracks_tests tests/companion_tracks_tests.cpp)
target_link_libraries(companion_tracks_tests PRIVATE gdx_companion_host)

add_executable(companion_music_tests tests/companion_music_tests.cpp)
target_link_libraries(companion_music_tests PRIVATE gdx_companion_host)
target_compile_definitions(companion_music_tests PRIVATE
    GDX_TEST_MUSIC_TARGETS="${CMAKE_SOURCE_DIR}/tools/music_targets.json"
)

add_executable(companion_loop_tests tests/companion_loop_tests.cpp)
target_link_libraries(companion_loop_tests PRIVATE gdx_companion_host)

add_executable(companion_project_tests tests/companion_project_tests.cpp)
target_link_libraries(companion_project_tests PRIVATE gdx_companion_host)

add_executable(companion_theme_tests tests/companion_theme_tests.cpp)
target_link_libraries(companion_theme_tests PRIVATE gdx_companion_host)

add_executable(companion_install_tests tests/companion_install_tests.cpp)
target_link_libraries(companion_install_tests PRIVATE gdx_companion_host)

# Job tests drive the real worker script with the build's Python, in temporary folders only.
find_package(Python3 COMPONENTS Interpreter)
add_executable(companion_job_tests tests/companion_job_tests.cpp)
target_link_libraries(companion_job_tests PRIVATE gdx_companion_host)
if(Python3_Interpreter_FOUND)
    target_compile_definitions(companion_job_tests PRIVATE
        GDX_TEST_PYTHON="${Python3_EXECUTABLE}"
        GDX_TEST_WORKER="${CMAKE_CURRENT_SOURCE_DIR}/worker/gdx_companion_worker.py"
    )
endif()

add_executable(G-Diffuser-Companion
    gdx_companion_backdrop.cpp
    gdx_companion_main.cpp
    gdx_companion_pages.cpp
    gdx_companion_preview.cpp
    gdx_companion_ui.cpp
)
if(WIN32)
    set_target_properties(G-Diffuser-Companion PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_link_libraries(G-Diffuser-Companion PRIVATE
    gdx_companion_host
    ImGui
    stb
    SDL2::SDL2
    OpenGL::GL
)
if(WIN32)
    # GetOpenFileNameW for Settings > Installation > Browse; IFileOpenDialog and IFileSaveDialog for
    # the workspace pages.
    target_link_libraries(G-Diffuser-Companion PRIVATE comdlg32 ole32 shell32 uuid)
endif()
# The Music page reads the native sample targets from beside the executable.
add_custom_command(TARGET G-Diffuser-Companion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/tools/music_targets.json
            $<TARGET_FILE_DIR:G-Diffuser-Companion>/music_targets.json
    VERBATIM
)

# The frozen worker, when tools/build_companion_worker.py has produced it, goes to <exe dir>/worker.
set(GDX_COMPANION_WORKER_SOURCE "${CMAKE_SOURCE_DIR}/build/companion-worker")
add_custom_target(G-Diffuser-Companion-worker ALL
    COMMAND ${CMAKE_COMMAND}
            -DGDX_WORKER_SOURCE=${GDX_COMPANION_WORKER_SOURCE}
            -DGDX_WORKER_DEST=$<TARGET_FILE_DIR:G-Diffuser-Companion>/worker
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyCompanionWorker.cmake
    VERBATIM
)
add_dependencies(G-Diffuser-Companion-worker G-Diffuser-Companion)
