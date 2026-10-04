# the settings app is a .net project; cmake runs its publish so one build makes everything, into
# the same folder as the engine.
find_program(DOTNET dotnet REQUIRED)

# native aot finds the msvc linker by running vcvarsall.bat and reading its output. visual studio
# 2026's vcvarsall calls vswhere.exe through PATH, and the visual studio installer folder that holds
# it is not always on PATH: the compiler then reads an error message instead of the linker's path.
# the publish therefore runs with that folder added.
set(program_files_x86 "ProgramFiles(x86)")
find_program(VSWHERE vswhere.exe
    PATHS "$ENV{${program_files_x86}}/Microsoft Visual Studio/Installer"
    REQUIRED)
cmake_path(GET VSWHERE PARENT_PATH vswhere_dir)
cmake_path(NATIVE_PATH vswhere_dir vswhere_dir)

add_custom_target(skyggn_app ALL
    COMMAND ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${vswhere_dir}"
            ${DOTNET} publish ${CMAKE_CURRENT_SOURCE_DIR}/skyggn.csproj
            --configuration $<CONFIG> --nologo --verbosity quiet
            --output ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "publishing the settings app (skyggn.exe)"
    VERBATIM)
add_dependencies(skyggn_app skyggn skyggnctl)
