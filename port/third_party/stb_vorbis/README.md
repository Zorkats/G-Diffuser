# stb_vorbis

Ogg Vorbis decoder, vendored for the streamed music replacement (`port/gdx_music_stream.cpp`).

- Version: v1.22 (from the file header)
- Upstream: https://github.com/nothings/stb (file `stb_vorbis.c`)
- Commit: `1ee679ca2ef753a528db5ba6801e1067b40481b8` ("update version numbers", 2021-07-12)
- Source file: `https://raw.githubusercontent.com/nothings/stb/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_vorbis.c`
- SHA-256 of `stb_vorbis.c`: `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`
- License: public domain, or MIT as an alternative (license text at the end of `stb_vorbis.c`)

The file is vendored unmodified. It is compiled from `port/third_party/music_decoders/decoders.c`, which
is the only translation unit that includes it, with warnings suppressed the same way as `gdx_lua`.
