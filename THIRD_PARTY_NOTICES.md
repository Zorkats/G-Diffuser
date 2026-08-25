# Third-party notices

G-Diffuser combines original port code with independently licensed decompilation, engine, tooling,
font, and support components. The corresponding license texts are distributed in `LICENSES/` and
must remain with binary packages.

## Core components

| Component | Use | License file |
|---|---|---|
| F-Zero X decompilation | Recreated game code and extraction recipes | `LICENSES/CC0-1.0.txt` |
| F-Zero X Expansion Kit decompilation | Source/reference submodule | `LICENSES/CC0-1.0.txt` |
| libultraship | Runtime, platform abstraction, and resource system | `LICENSES/MIT-libultraship.txt` |
| Fast3D | Graphics interpreter and shaders | `LICENSES/MIT-Fast3D.txt` |
| Torch | `gdx-extract` asset processor | `LICENSES/MIT-Torch.txt` |
| cxd4 RSP core | Low-level audio RSP interpreter | `LICENSES/CC0-1.0.txt` |
| SSE2NEON | ARM SIMD compatibility header used by cxd4 | `LICENSES/MIT-SSE2NEON.txt` |
| discord-rpc | Discord Rich Presence (optional, off by default) | `LICENSES/MIT-discord-rpc.txt` |
| RapidJSON | JSON serialization inside discord-rpc | `LICENSES/MIT-rapidjson.txt` |
| Lua | Embedded scripting language runtime | `LICENSES/MIT-Lua.txt` |

## Runtime and extraction dependencies

The package also includes or statically links Dear ImGui, prism, BS::thread_pool, Monocypher, stb,
libgfxd, yaml-cpp, spdlog, tinyxml2, SDL2, fmt, libzip, zlib, bzip2, GLEW, nlohmann/json, and
miniz-cpp/miniz. Their complete notices are the correspondingly named files under `LICENSES/`.

Montserrat and Inconsolata are distributed under the SIL Open Font License 1.1. Their license texts
are included both under `LICENSES/` and beside the fonts in release packages.

`gamecontrollerdb.txt` is the community SDL_GameControllerDB, carrying the same zlib license as SDL
itself (`LICENSES/zlib-SDL2.txt`). It ships beside the executable and is layered on top of the
mappings built into SDL, so a controller newer than the bundled SDL can be made to work by replacing
that one file.

## Asset boundary

- G-Diffuser distributions do not include Nintendo ROM, disk, IPL, texture, model, audio, or other
  game payloads.
- `decomp-recipes/` contains CC0-licensed extraction metadata and no embedded game payload.
- `fzerox.o2r` is generated locally from files supplied by the user and must not be redistributed
  with G-Diffuser.
- RSP microcode (aspMain) is not shipped in the source tree or in binaries; the low-level audio
  path reads it from `fzerox.o2r`, where extraction places it from the user's own ROM.
- `gdiffuser.o2r` contains only the MIT-licensed Fast3D shaders required to initialize the renderer.
- The Input Viewer is drawn with ImGui primitives. No Ship of Harkinian controller PNGs are included.
- No game-derived application icon is included. Builds use the platform default until original,
  separately cleared artwork is provided.

F-Zero and Nintendo are trademarks of Nintendo. Their names are used only to identify compatibility;
no affiliation, endorsement, or sponsorship is claimed.
