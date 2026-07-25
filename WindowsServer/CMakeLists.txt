cmake_minimum_required(VERSION 3.14)
project(AudioSkidServer C)

if(NOT MSVC)
    message(FATAL_ERROR "This project only supports MSVC.")
endif()

set(CMAKE_C_FLAGS_RELEASE "/O2 /arch:AVX2 /fp:precise")

include(FetchContent)

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.6.1
)
FetchContent_MakeAvailable(opus)

add_executable(AudioSkidServer main.c)

target_link_libraries(AudioSkidServer
    ws2_32
    ole32
    mmdevapi
    avrt
    opus
)