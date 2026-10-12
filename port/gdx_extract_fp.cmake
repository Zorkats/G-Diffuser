# Injected into the gdx-extract (Torch) ExternalProject via CMAKE_PROJECT_INCLUDE, which CMake
# includes right after each project() call. See the gdx_extract_ext comment in port/CMakeLists.txt.
#
# This must be add_compile_options, not CMAKE_CXX_FLAGS: torch/CMakeLists.txt overwrites
# CMAKE_CXX_FLAGS with a plain set(), which silently discarded flags passed in from outside.
# add_compile_options sets a directory property that the overwrite does not touch.
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
