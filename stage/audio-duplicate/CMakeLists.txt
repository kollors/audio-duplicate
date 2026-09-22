cmake_minimum_required(VERSION 3.24)
project(AudioDuplicate VERSION 0.1.2 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(AudioDuplicate WIN32
    src/main.cpp
    src/audio_engine.cpp
    src/audio_engine.h
    src/settings.cpp
    src/settings.h
)

target_compile_definitions(AudioDuplicate PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

target_link_libraries(AudioDuplicate PRIVATE
    ole32
    uuid
    avrt
    comctl32
    dwmapi
    shell32
    propsys
)

if(MSVC)
  target_compile_options(AudioDuplicate PRIVATE /W4 /permissive- /utf-8)
endif()
