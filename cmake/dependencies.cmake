include(FetchContent)

# ffmpeg 9.0.2, lgpl-3.0 shared build by btbn. the last build of each month is kept for two years
# (https://github.com/BtbN/FFmpeg-Builds#release-retention-policy), so pin a month-end build.
FetchContent_Declare(ffmpeg
    URL https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0.zip
    URL_HASH SHA256=7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# windows implementation library, header-only. SOURCE_SUBDIR points nowhere so wil's own build
# (tests, nuget packaging) is skipped.
FetchContent_Declare(wil
    URL https://github.com/microsoft/wil/archive/refs/tags/v1.0.260126.7.tar.gz
    URL_HASH SHA256=de9e03b38ff0ff8d22048f00b111cb631d21c550328f12530ccba71c05c9e361
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none)

# libraw 0.22.2 reads camera raw files; skyggn uses it for the preview every camera embeds.
# lgpl-2.1 or cddl-1.0. it has no cmake build, so the target is defined below.
FetchContent_Declare(libraw
    URL https://github.com/LibRaw/LibRaw/archive/refs/tags/0.22.2.tar.gz
    URL_HASH SHA256=627928088300ecde6ca91ffd202e189203f04ad61ad12f0fe9dc57b9a7a0fb3c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(ffmpeg wil libraw)

file(GLOB_RECURSE libraw_sources CONFIGURE_DEPENDS ${libraw_SOURCE_DIR}/src/*.cpp)
# the *_ph.cpp files are stubs for a build without postprocessing (their dcraw_process returns "not
# implemented"); libraw's own makefiles leave them out, and they clash with the real functions
list(FILTER libraw_sources EXCLUDE REGEX "_ph\\.cpp$")
add_library(libraw STATIC ${libraw_sources})
target_include_directories(libraw SYSTEM PUBLIC ${libraw_SOURCE_DIR})
# USE_X3FTOOLS switches on sigma's x3f files, which libraw otherwise leaves out
target_compile_definitions(libraw PUBLIC LIBRAW_NODLL PRIVATE USE_X3FTOOLS _CRT_SECURE_NO_WARNINGS)
# third-party code: its own warnings are not ours to fix, and it predates msvc's strict mode
target_compile_options(libraw PRIVATE /W0 /permissive)
target_link_libraries(libraw PUBLIC ws2_32)

add_library(wil INTERFACE)
target_include_directories(wil SYSTEM INTERFACE ${wil_SOURCE_DIR}/include)

# libarchive opens comic book archives (zip, rar, 7z, tar) and e-books (zip); zlib and xz give it
# deflate and lzma. libarchive's own configure step test-links against zlib and xz, so the three
# are separate cmake builds, run in order at build time, each in release and debug with the static
# c runtime, so either configuration of skyggn links them. debug libraries end in "d".
#
# the chain takes many minutes, mostly libarchive's configure checks, so its result is kept: the
# install folder records the versions it holds, and a build folder (or ci cache) that already has
# these versions skips the chain.
set(zlib_version 1.3.2)
set(xz_version 5.8.4)
set(libarchive_version 3.8.9)
set(archive_versions "zlib ${zlib_version}, xz ${xz_version}, libarchive ${libarchive_version}")
set(archive_prefix ${CMAKE_BINARY_DIR}/_deps/archive-install)
set(archive_stamp ${archive_prefix}/versions.txt)
set(SKYGGN_ARCHIVE_LICENSES ${archive_prefix}/licenses)
set(built_versions "")
if(EXISTS ${archive_stamp})
    file(READ ${archive_stamp} built_versions)
endif()

add_library(libarchive INTERFACE)
target_include_directories(libarchive SYSTEM INTERFACE ${archive_prefix}/include)
target_compile_definitions(libarchive INTERFACE LIBARCHIVE_STATIC)
foreach(library IN ITEMS archive lzma zs)
    target_link_libraries(libarchive INTERFACE ${archive_prefix}/lib/${library}$<$<CONFIG:Debug>:d>.lib)
endforeach()
# libarchive names its temporary files with windows' random number api, whatever ENABLE_CNG says
target_link_libraries(libarchive INTERFACE bcrypt)

if(NOT built_versions STREQUAL archive_versions)
    include(ExternalProject)
    set(archive_cache ${CMAKE_BINARY_DIR}/_deps/archive-cache.cmake)
    file(WRITE ${archive_cache} "
set(CMAKE_INSTALL_PREFIX \"${archive_prefix}\" CACHE PATH \"\")
set(CMAKE_PREFIX_PATH \"${archive_prefix}\" CACHE PATH \"\")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW CACHE STRING \"\")
set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\" CACHE STRING \"\")
set(CMAKE_DEBUG_POSTFIX d CACHE STRING \"\")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release CACHE STRING \"\")
set(BUILD_SHARED_LIBS OFF CACHE BOOL \"\")
set(BUILD_TESTING OFF CACHE BOOL \"\")
")

    # builds one library and installs it, with its license file, into archive_prefix
    function(skyggn_archive_dependency name url hash license)
        cmake_parse_arguments(PARSE_ARGV 4 arg "" "" "OPTIONS;DEPENDS;LIBRARY")
        set(source_dir ${CMAKE_BINARY_DIR}/_deps/${name}-src)
        ExternalProject_Add(${name}
            URL ${url}
            URL_HASH SHA256=${hash}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_DIR ${source_dir}
            BINARY_DIR ${CMAKE_BINARY_DIR}/_deps/${name}-build
            DEPENDS ${arg_DEPENDS}
            CMAKE_ARGS -C ${archive_cache} ${arg_OPTIONS}
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
                  COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Debug
            INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
                    COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Debug
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${SKYGGN_ARCHIVE_LICENSES}
                    COMMAND ${CMAKE_COMMAND} -E copy ${source_dir}/${license}
                            ${SKYGGN_ARCHIVE_LICENSES}/${name}-license.txt
            BUILD_BYPRODUCTS ${archive_prefix}/lib/${arg_LIBRARY}.lib ${archive_prefix}/lib/${arg_LIBRARY}d.lib)
    endfunction()

    # zlib license
    skyggn_archive_dependency(zlib
        https://github.com/madler/zlib/releases/download/v${zlib_version}/zlib-${zlib_version}.tar.gz
        bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
        LICENSE
        LIBRARY zs
        OPTIONS -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF)

    # xz's liblzma is 0bsd
    skyggn_archive_dependency(xz
        https://github.com/tukaani-project/xz/releases/download/v${xz_version}/xz-${xz_version}.tar.gz
        0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
        COPYING.0BSD
        LIBRARY lzma
        OPTIONS -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF
                -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF)

    # bsd-2-clause; only the reader's formats and the two decompressors are used
    skyggn_archive_dependency(libarchive_build
        https://github.com/libarchive/libarchive/releases/download/v${libarchive_version}/libarchive-${libarchive_version}.tar.xz
        888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde
        COPYING
        LIBRARY archive
        DEPENDS zlib xz
        OPTIONS -DMSVC_USE_STATIC_CRT=ON -DZLIB_USE_STATIC_LIBS=ON -DENABLE_TEST=OFF -DENABLE_TAR=OFF
                -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_OPENSSL=OFF -DENABLE_CNG=OFF
                -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF -DENABLE_LIBXML2=OFF
                -DENABLE_EXPAT=OFF -DENABLE_WIN32_XMLLITE=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF
                -DENABLE_ICONV=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DPOSIX_REGEX_LIB=NONE)

    # the stamp is written last, so a chain that stopped halfway runs again
    file(WRITE ${CMAKE_BINARY_DIR}/_deps/archive-versions.txt "${archive_versions}")
    ExternalProject_Add_Step(libarchive_build stamp
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/_deps/archive-versions.txt ${archive_stamp}
        DEPENDEES install)
    add_dependencies(libarchive libarchive_build)
endif()

# the ffmpeg libraries the engine calls directly. the engine delay-loads their dlls from its own
# folder (see engine/src/ffmpeg_loader.cpp), so their names are collected here.
set(SKYGGN_FFMPEG_LIBS avformat avcodec avutil swscale)
# every ffmpeg dll needed at runtime: the libraries above plus their own dependencies.
set(SKYGGN_FFMPEG_RUNTIME_LIBS ${SKYGGN_FFMPEG_LIBS} swresample)

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg SYSTEM INTERFACE ${ffmpeg_SOURCE_DIR}/include)

# the build ships mingw-made import libraries, which msvc's /DELAYLOAD does not recognise ("no
# imports found"). lib.exe regenerates them from the shipped .def files in msvc's own format.
set(implib_dir ${CMAKE_BINARY_DIR}/ffmpeg-import-libs)
file(MAKE_DIRECTORY ${implib_dir})
set(SKYGGN_FFMPEG_DELAYLOAD_DLLS)
foreach(lib IN LISTS SKYGGN_FFMPEG_LIBS)
    file(GLOB dll RELATIVE ${ffmpeg_SOURCE_DIR}/bin ${ffmpeg_SOURCE_DIR}/bin/${lib}-*.dll)
    get_filename_component(dll_name ${dll} NAME_WE)
    execute_process(
        COMMAND ${CMAKE_AR} /nologo /machine:x64 /def:${ffmpeg_SOURCE_DIR}/lib/${dll_name}.def /name:${dll}
                /out:${implib_dir}/${lib}.lib
        COMMAND_ERROR_IS_FATAL ANY
        OUTPUT_QUIET)
    target_link_libraries(ffmpeg INTERFACE ${implib_dir}/${lib}.lib)
    list(APPEND SKYGGN_FFMPEG_DELAYLOAD_DLLS ${dll})
endforeach()

set(SKYGGN_FFMPEG_RUNTIME_DLLS)
foreach(lib IN LISTS SKYGGN_FFMPEG_RUNTIME_LIBS)
    file(GLOB dll ${ffmpeg_SOURCE_DIR}/bin/${lib}-*.dll)
    if(NOT dll)
        message(FATAL_ERROR "ffmpeg build has no ${lib} dll in ${ffmpeg_SOURCE_DIR}/bin")
    endif()
    list(APPEND SKYGGN_FFMPEG_RUNTIME_DLLS ${dll})
endforeach()

set(SKYGGN_FFMPEG_LICENSE ${ffmpeg_SOURCE_DIR}/LICENSE.txt)
