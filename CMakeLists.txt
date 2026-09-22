cmake_minimum_required(VERSION 3.21)
project(AudioDuplicate VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(AudioDuplicate WIN32
    src/main.cpp
    src/audio.cpp
    src/config.cpp
    res/resources.rc
)

target_include_directories(AudioDuplicate PRIVATE src)
target_compile_definitions(AudioDuplicate PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(AudioDuplicate PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

target_link_libraries(AudioDuplicate PRIVATE
    ole32
    uuid
    propsys
    avrt
    dwmapi
    uxtheme
    comctl32
)

set_target_properties(AudioDuplicate PROPERTIES
    OUTPUT_NAME "AudioDuplicate"
)
