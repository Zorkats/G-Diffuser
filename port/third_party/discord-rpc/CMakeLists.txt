# discord-rpc, vendored from https://github.com/discord/discord-rpc at
# 963aa9f3e5ce81a4682c6ca3d136cddda614db33 (upstream master HEAD, 2020-09-21;
# the commit OpenGOAL ships too). Stripped to the library sources: no examples,
# no CI, no build.py, no dllmain (static build only). rapidjson 1.1.0 headers
# are committed under thirdparty/ because upstream file(DOWNLOAD)s them at
# configure time, which a reproducible build cannot accept. MIT licensed
# (LICENSE here, thirdparty/rapidjson-license.txt).
#
# The IO thread stays enabled (upstream default): the library owns reconnect
# backoff (src/backoff.h, 500ms..60s jittered) on a background thread, so a
# machine without Discord running pays one non-blocking pipe-open per backoff
# interval and nothing on the frame loop.

add_library(discord-rpc STATIC
    src/discord_rpc.cpp
    src/rpc_connection.cpp
    src/serialization.cpp
    $<$<BOOL:${WIN32}>:src/connection_win.cpp>
    $<$<BOOL:${WIN32}>:src/discord_register_win.cpp>
    $<$<NOT:$<BOOL:${WIN32}>>:src/connection_unix.cpp>
    $<$<NOT:$<BOOL:${WIN32}>>:src/discord_register_linux.cpp>
)

target_include_directories(discord-rpc PUBLIC include)
target_include_directories(discord-rpc PRIVATE thirdparty)

if(MSVC)
    # /MT comes from CMAKE_MSVC_RUNTIME_LIBRARY at the root. /W0 silences the
    # vendored code's warnings without patching upstream sources.
    target_compile_options(discord-rpc PRIVATE /W0)
else()
    target_compile_options(discord-rpc PRIVATE -w)
    # Defense in depth: GCC >= 14 still hard-errors a restored vanilla 1.1.0
    # GenericStringRef::operator=. The vendored document.h deletes that operator
    # so Clang 22 can compile; this flag covers a header revert on GCC.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
        target_compile_options(discord-rpc PRIVATE -Wno-template-body)
    endif()
endif()
