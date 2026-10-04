# the installer: an inno setup 6 script, built on demand with
#   cmake --build --preset release --target installer
# into build/dist. it packs the release output, so it always uses the release configuration.

set(program_files_x86 "ProgramFiles(x86)")
find_program(ISCC ISCC.exe PATHS
    "$ENV{${program_files_x86}}/Inno Setup 6"
    "$ENV{ProgramFiles}/Inno Setup 6"
    "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6")

set(installer_dir ${CMAKE_CURRENT_BINARY_DIR})
set(dist_dir ${CMAKE_BINARY_DIR}/dist)
set(ffmpeg_exe ${ffmpeg_SOURCE_DIR}/bin/ffmpeg.exe)
set(icon_svg ${PROJECT_SOURCE_DIR}/assets/skyggn.svg)

# the wizard's pictures are the app icon, rendered from its svg
add_custom_command(
    OUTPUT ${installer_dir}/wizard-small.png ${installer_dir}/wizard-image.png
    COMMAND ${CMAKE_COMMAND} -E make_directory ${installer_dir}
    COMMAND ${ffmpeg_exe} -hide_banner -loglevel error -y -width 1024 -height 1024 -i ${icon_svg}
            -vf scale=110:110:flags=lanczos,format=rgba -frames:v 1 ${installer_dir}/wizard-small.png
    COMMAND ${ffmpeg_exe} -hide_banner -loglevel error -y -width 1024 -height 1024 -i ${icon_svg}
            -vf "scale=300:300:flags=lanczos,format=rgba,pad=430:824:(ow-iw)/2:(oh-ih)/2:color=0x00000000"
            -frames:v 1 ${installer_dir}/wizard-image.png
    DEPENDS ${icon_svg}
    VERBATIM)

if(ISCC)
    add_custom_target(installer
        COMMAND ${ISCC} /Qp
                /DAppVersion=${PROJECT_VERSION}
                /DSourceDir=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release
                /DImageDir=${installer_dir}
                /DOutputDir=${dist_dir}
                /DRepoDir=${PROJECT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/skyggn.iss
        DEPENDS ${installer_dir}/wizard-small.png ${installer_dir}/wizard-image.png
        COMMENT "building build/dist/skyggn-${PROJECT_VERSION}-x64-setup.exe"
        VERBATIM)
else()
    add_custom_target(installer
        COMMAND ${CMAKE_COMMAND} -E echo "installer: inno setup 6 (ISCC.exe) not found; install it with: winget install JRSoftware.InnoSetup"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
endif()
add_dependencies(installer skyggn_app)
