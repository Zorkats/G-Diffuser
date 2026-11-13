# Standalone regression harness: no renderer, SDL SDK, downloaded packages, or root configure needed.
cmake_minimum_required(VERSION 3.20)
project(GdxPhysicalInputTests LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(GDX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SHIP_MAPPING "${GDX_ROOT}/libultraship/src/ship/controller/controldevice/controller/mapping")
set(GDX_PHYSICAL_RUNTIME_SOURCES
    "${GDX_ROOT}/port/gdx_physical_input.cpp"
    "${GDX_ROOT}/port/gdx_input_actions.c"
    "${SHIP_MAPPING}/ControllerMapping.cpp"
    "${SHIP_MAPPING}/ControllerInputMapping.cpp"
    "${SHIP_MAPPING}/ControllerButtonMapping.cpp"
    "${SHIP_MAPPING}/keyboard/KeyboardKeyToAnyMapping.cpp"
    "${SHIP_MAPPING}/keyboard/KeyboardKeyToButtonMapping.cpp"
    "${SHIP_MAPPING}/sdl/SDLButtonToAnyMapping.cpp"
    "${SHIP_MAPPING}/sdl/SDLButtonToButtonMapping.cpp"
    "${SHIP_MAPPING}/sdl/SDLAxisDirectionToAnyMapping.cpp"
    "${SHIP_MAPPING}/sdl/SDLAxisDirectionToButtonMapping.cpp"
    "${SHIP_MAPPING}/mouse/MouseButtonToAnyMapping.cpp"
    "${SHIP_MAPPING}/mouse/MouseButtonToButtonMapping.cpp"
    "${SHIP_MAPPING}/mouse/MouseWheelToAnyMapping.cpp"
    "${SHIP_MAPPING}/mouse/MouseWheelToButtonMapping.cpp"
    "${SHIP_MAPPING}/factories/ButtonMappingFactory.cpp")
add_executable(gdx_physical_input_tests
    gdx_physical_input_tests.cpp
    gdx_physical_input_lag_tests.cpp
    gdx_physical_input_replace_tests.cpp
    ${GDX_PHYSICAL_RUNTIME_SOURCES})
# The fixtures shadow services, not the production mapping classes or implementations.
target_include_directories(gdx_physical_input_tests PRIVATE
    include "${GDX_ROOT}/libultraship/include" "${GDX_ROOT}/port" "${GDX_ROOT}/decomp/include"
    "${GDX_ROOT}/port/third_party/discord-rpc/thirdparty")

add_executable(gdx_input_actions_tests
    "${GDX_ROOT}/port/tests/gdx_input_actions_tests.c"
    "${GDX_ROOT}/port/gdx_input_actions.c")
target_include_directories(gdx_input_actions_tests PRIVATE "${GDX_ROOT}/port" "${GDX_ROOT}/decomp/include")
# Optional, local-only ImGui discovery. Never configure the game or fetch dependencies.
option(GDX_TEST_HEADLESS_IMGUI "Build headless input editor tests when cached ImGui is available" ON)
set(GDX_TEST_IMGUI_DIR "" CACHE PATH "Existing ImGui source directory (no downloads)")
set(GDX_TEST_TARGETS gdx_physical_input_tests gdx_input_actions_tests)
if(GDX_TEST_HEADLESS_IMGUI)
    if(NOT GDX_TEST_IMGUI_DIR)
        foreach(CANDIDATE IN ITEMS "${GDX_ROOT}/build/x64/_deps/imgui-src" "${GDX_ROOT}/build/_deps/imgui-src")
            if(EXISTS "${CANDIDATE}/imgui.cpp")
                set(GDX_TEST_IMGUI_DIR "${CANDIDATE}")
                break()
            endif()
        endforeach()
    endif()
    set(GDX_TEST_IMGUI_FOUND TRUE)
    foreach(IMGUI_FILE IN ITEMS imgui.h imgui_internal.h imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp)
        if(NOT EXISTS "${GDX_TEST_IMGUI_DIR}/${IMGUI_FILE}")
            set(GDX_TEST_IMGUI_FOUND FALSE)
        endif()
    endforeach()
    if(GDX_TEST_IMGUI_FOUND)
        add_executable(gdx_input_editor_tests
            gdx_input_editor_tests.cpp
            "${GDX_ROOT}/port/gdx_input_editor.cpp"
            ${GDX_PHYSICAL_RUNTIME_SOURCES}
            "${GDX_TEST_IMGUI_DIR}/imgui.cpp"
            "${GDX_TEST_IMGUI_DIR}/imgui_draw.cpp"
            "${GDX_TEST_IMGUI_DIR}/imgui_tables.cpp"
            "${GDX_TEST_IMGUI_DIR}/imgui_widgets.cpp")
        target_include_directories(gdx_input_editor_tests PRIVATE
            ui_include include "${GDX_TEST_IMGUI_DIR}" "${GDX_ROOT}/libultraship/include"
            "${GDX_ROOT}/port" "${GDX_ROOT}/decomp/include"
            "${GDX_ROOT}/port/third_party/discord-rpc/thirdparty")
        target_compile_definitions(gdx_input_editor_tests PRIVATE IMGUI_ENABLE_TEST_ENGINE)
        list(APPEND GDX_TEST_TARGETS gdx_input_editor_tests)
        message(STATUS "Headless input editor tests: ${GDX_TEST_IMGUI_DIR}")
    else()
        message(STATUS "Skipping headless input editor tests: cached ImGui unavailable (set GDX_TEST_IMGUI_DIR)")
    endif()
endif()
foreach(TEST_TARGET IN LISTS GDX_TEST_TARGETS)
    target_compile_definitions(${TEST_TARGET} PRIVATE PORT=1 GDIFFUSER_PORT=1 VERSION_US=1)
    if(MSVC)
        target_compile_options(${TEST_TARGET} PRIVATE /W3)
        target_compile_definitions(${TEST_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${TEST_TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(NOT TEST_TARGET STREQUAL "gdx_input_editor_tests")
        add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
    endif()
endforeach()
if(TARGET gdx_input_editor_tests)
    foreach(UI_CASE IN ITEMS add_save cancel controller_capture rebind_remove isolation hidden collapsed heartbeat destructor profiles)
        add_test(NAME gdx_input_editor_${UI_CASE} COMMAND gdx_input_editor_tests ${UI_CASE})
    endforeach()
endif()
target_compile_definitions(gdx_input_actions_tests PRIVATE _LANGUAGE_C=1)
add_test(NAME gdx_physical_input_bridge_contract
    COMMAND "${CMAKE_COMMAND}" "-DGDX_ROOT=${GDX_ROOT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/bridge_contract.cmake")
