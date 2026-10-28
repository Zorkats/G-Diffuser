# gdx-update: the self-update helper. The game copies it to <install>/.update/ and runs the copy,
# so the helper can replace the installed executables (gdx_update.cpp starts it, then quits).
# libzip is found again here because libultraship's find_package result is directory-scoped.
find_package(libzip REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(gdx-update WIN32
    gdx_update_main.cpp
    gdx_update_apply.cpp)
target_compile_definitions(gdx-update PRIVATE GDX_UPDATE_HAVE_ARCHIVES=1)
target_link_libraries(gdx-update PRIVATE libzip::zip ZLIB::ZLIB)
if(WIN32)
    target_compile_definitions(gdx-update PRIVATE UNICODE _UNICODE)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx-update PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
if(UNIX AND NOT APPLE)
    # The release tarball bundles zlib into lib/ on some targets (linux-arm64), so the helper needs
    # a $ORIGIN RUNPATH like G-Diffuser's. It runs as a copy in <install>/.update/, hence ../lib;
    # $ORIGIN/lib covers running it from the install directory itself.
    set_target_properties(gdx-update PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib;$ORIGIN/../lib")
endif()
