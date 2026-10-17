# dr_libs (dr_flac, dr_wav)

FLAC and WAV decoders, vendored for the streamed music replacement (`port/gdx_music_stream.cpp`).

| File | Version (file header) | Upstream commit | Date |
| --- | --- | --- | --- |
| `dr_flac.h` | v0.13.4 | `dfe8377631000664666519fdb83da193fd8037f4` ("dr_flac: Fix a seeking error.") | 2026-08-31 |
| `dr_wav.h` | v0.14.6 | `019e75ba8e9ac47222dfb87671029812069f68dc` ("dr_wav: Improve float-to-int sample conversion.") | 2026-08-31 |

- Upstream: https://github.com/mackron/dr_libs
- Source URLs: `https://raw.githubusercontent.com/mackron/dr_libs/<commit>/dr_flac.h` and `.../dr_wav.h`
- SHA-256 `dr_flac.h`: `111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c`; SHA-256 `dr_wav.h`: `03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493`
- License: public domain, or MIT-0 as an alternative (license text at the end of each header)

Both files are vendored unmodified. They are compiled from `port/third_party/music_decoders/decoders.c`
with `DR_FLAC_IMPLEMENTATION` and `DR_WAV_IMPLEMENTATION` defined there, and warnings are suppressed the
same way as `gdx_lua`.
