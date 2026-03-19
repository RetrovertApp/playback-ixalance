# Minimal vendored zlib build (inflate/deflate only, no gzip file I/O)
# Guard against duplicate target when multiple plugins vendor zlib in the same build tree
if(NOT TARGET zlib_static)
    add_library(zlib_static STATIC
        adler32.c
        compress.c
        crc32.c
        deflate.c
        infback.c
        inffast.c
        inflate.c
        inftrees.c
        trees.c
        uncompr.c
        zutil.c
    )
    target_include_directories(zlib_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(zlib_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Provide ZLIB::ZLIB target so plugins can use standard CMake convention
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib_static)
endif()
