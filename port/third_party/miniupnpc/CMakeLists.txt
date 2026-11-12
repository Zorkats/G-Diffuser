# miniupnpc 2.3.3, the UPnP IGD client G-NET asks a router for a port mapping with (decision 58). Vendored from
# https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz
# (sha256 8cf2c833b3e76fc4893ff29c2a376e3394962449e5970e373c0a91421724d222). BSD 3-Clause (LICENSE here,
# LICENSES/BSD-3-Clause-miniupnpc.txt in release packages). Only the library is vendored: include/, the sources
# upstream's CMakeLists.txt lists for the library, and their private headers. This file replaces upstream's, which
# also builds samples, tests, a shared library and install rules; the definitions below are the ones it sets.

set(PROJECT_VERSION 2.3.3) # read by miniupnpcstrings.h.cmake; this directory's scope only
configure_file(miniupnpcstrings.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/miniupnpcstrings.h)

add_library(gdx_miniupnpc STATIC
    src/addr_is_reserved.c
    src/connecthostport.c
    src/igd_desc_parse.c
    src/minisoap.c
    src/minissdpc.c
    src/miniupnpc.c
    src/miniwget.c
    src/minixml.c
    src/portlistingparse.c
    src/receivedata.c
    src/upnpcommands.c
    src/upnpdev.c
    src/upnperrors.c
    src/upnpreplyparse.c
)

target_include_directories(gdx_miniupnpc PUBLIC include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(gdx_miniupnpc PUBLIC MINIUPNP_STATICLIB)

if(WIN32)
    target_compile_definitions(gdx_miniupnpc PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    target_link_libraries(gdx_miniupnpc PUBLIC ws2_32 iphlpapi)
else()
    target_compile_definitions(gdx_miniupnpc PRIVATE MINIUPNPC_SET_SOCKET_TIMEOUT MINIUPNPC_GET_SRC_ADDR
                                                      _BSD_SOURCE _DEFAULT_SOURCE)
    if(APPLE)
        target_compile_definitions(gdx_miniupnpc PRIVATE _DARWIN_C_SOURCE)
    else()
        target_compile_definitions(gdx_miniupnpc PRIVATE _XOPEN_SOURCE=600)
    endif()
    set_target_properties(gdx_miniupnpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(MSVC)
    # /W0 silences the vendored code's warnings without patching upstream sources.
    target_compile_options(gdx_miniupnpc PRIVATE /W0)
else()
    target_compile_options(gdx_miniupnpc PRIVATE -w)
endif()
