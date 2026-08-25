# Lua 5.4.8, vendored from https://www.lua.org/ftp/lua-5.4.8.tar.gz
# (sha256 4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae,
# verified against the checksum column of https://www.lua.org/ftp/). MIT licensed
# (LICENSE here, LICENSES/MIT-Lua.txt in release packages). The complete src/
# is vendored minus the standalone interpreters (lua.c, luac.c, onelua.c) — the
# host has no main() — and including liolib.c/loslib.c: they compile but are
# never registered, and stripping them would turn every upstream upgrade into a
# source-list edit.

add_library(gdx_lua STATIC
    src/lapi.c
    src/lauxlib.c
    src/lbaselib.c
    src/lcode.c
    src/lcorolib.c
    src/lctype.c
    src/ldblib.c
    src/ldebug.c
    src/ldo.c
    src/ldump.c
    src/lfunc.c
    src/lgc.c
    src/linit.c
    src/liolib.c
    src/llex.c
    src/lmathlib.c
    src/lmem.c
    src/loadlib.c
    src/lobject.c
    src/lopcodes.c
    src/loslib.c
    src/lparser.c
    src/lstate.c
    src/lstring.c
    src/lstrlib.c
    src/ltable.c
    src/ltablib.c
    src/ltm.c
    src/lundump.c
    src/lutf8lib.c
    src/lvm.c
    src/lzio.c
)

target_include_directories(gdx_lua PUBLIC src)

if(MSVC)
    # /W0 silences the vendored code's warnings without patching upstream sources.
    target_compile_options(gdx_lua PRIVATE /W0)
else()
    target_compile_options(gdx_lua PRIVATE -w)
endif()
