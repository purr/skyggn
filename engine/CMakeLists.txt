set(SKYGGN_FFMPEG_DLL_LIST "")
foreach(dll IN LISTS SKYGGN_FFMPEG_DELAYLOAD_DLLS)
    string(APPEND SKYGGN_FFMPEG_DLL_LIST "\"${dll}\", ")
endforeach()

set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/build_info.h.in ${generated}/build_info.h @ONLY)
configure_file(src/version.rc.in ${generated}/version.rc @ONLY)

add_library(skyggn SHARED
    src/api.cpp
    src/badge.cpp
    src/byte_reader.cpp
    src/damage.cpp
    src/decoder.cpp
    src/design.cpp
    src/details.cpp
    src/ffmpeg_loader.cpp
    src/fingerprint.cpp
    src/formats.cpp
    src/image.cpp
    src/media_input.cpp
    src/module.cpp
    src/package.cpp
    src/pdf.cpp
    src/prepare.cpp
    src/properties.cpp
    src/provider.cpp
    src/raw.cpp
    src/registration.cpp
    src/settings.cpp
    src/text.cpp
    src/tonemap.cpp
    src/wic.cpp
    src/resources.rc
    src/skyggn.def
    ${generated}/version.rc)

# assets/ is on the include path so resources.rc finds the badge svgs
# skyggn-engine.dll: skyggn.exe is the settings app
set_target_properties(skyggn PROPERTIES OUTPUT_NAME skyggn-engine)

target_include_directories(skyggn
    PUBLIC include
    PRIVATE src assets ${generated})
target_compile_definitions(skyggn PRIVATE SKYGGN_BUILDING)
target_link_libraries(skyggn PRIVATE ffmpeg wil libraw libarchive xmllite propsys shcore delayimp runtimeobject shlwapi pathcch d2d1 dwrite windowscodecs dxguid bcrypt)

foreach(dll IN LISTS SKYGGN_FFMPEG_DELAYLOAD_DLLS)
    target_link_options(skyggn PRIVATE /DELAYLOAD:${dll})
endforeach()

# ffmpeg's dlls sit next to the engine; every license the install needs goes to licenses/
set(licenses $<TARGET_FILE_DIR:skyggn>/licenses)
add_custom_command(TARGET skyggn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYGGN_FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:skyggn>
    COMMAND ${CMAKE_COMMAND} -E make_directory ${licenses}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/LICENSE ${licenses}/skyggn-license.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md ${licenses}/third-party-notices.md
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYGGN_FFMPEG_LICENSE} ${licenses}/ffmpeg-license.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${libraw_SOURCE_DIR}/LICENSE.LGPL ${licenses}/libraw-license.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${libraw_SOURCE_DIR}/COPYRIGHT ${licenses}/libraw-copyright.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYGGN_ARCHIVE_LICENSES}/libarchive_build-license.txt ${licenses}/libarchive-license.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYGGN_ARCHIVE_LICENSES}/zlib-license.txt ${licenses}/zlib-license.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYGGN_ARCHIVE_LICENSES}/xz-license.txt ${licenses}/xz-license.txt
    VERBATIM)
