# Ogg Vorbis, FLAC and WAV decoders for the streamed BGM replacement (port/gdx_music_stream.cpp).
# decoders.c is the one translation unit that holds the vendored stb_vorbis, dr_flac and dr_wav
# implementations. Provenance is in port/third_party/stb_vorbis/README.md and
# port/third_party/dr_libs/README.md.

add_library(gdx_music_decoders STATIC
    decoders.c
)

target_include_directories(gdx_music_decoders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    # /W0 silences the vendored code's warnings without patching upstream sources, like gdx_lua.
    target_compile_options(gdx_music_decoders PRIVATE /W0)
else()
    target_compile_options(gdx_music_decoders PRIVATE -w)
endif()
