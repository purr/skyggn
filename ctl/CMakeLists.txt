add_executable(skyggnctl src/main.cpp)
target_link_libraries(skyggnctl PRIVATE skyggn wil shlwapi windowscodecs propsys)
