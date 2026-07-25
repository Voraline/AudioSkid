cmake_minimum_required(VERSION 3.22.1)
project("audio-skid")

set(CMAKE_BUILD_TYPE Release)

include(FetchContent)

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_MAY_HAVE_NEON OFF CACHE BOOL "" FORCE)
set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.6.1
)
FetchContent_MakeAvailable(opus)

add_library(audio-skid SHARED
    native-lib.c
)

target_compile_options(audio-skid PRIVATE
    -std=c11
    -O3
    -march=armv8.2-a+simd
    -mtune=cortex-a76
    -flto=thin
    -funroll-loops
    -fomit-frame-pointer
    -fno-plt
    -fno-semantic-interposition
    -fno-common
    -fno-math-errno
    -ffp-contract=on
    -fno-stack-protector
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -DNDEBUG
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
)

target_link_options(audio-skid PRIVATE
    -flto=thin
    -Wl,--gc-sections
    -Wl,-O3
    -Wl,--as-needed
    -Wl,--icf=all
    -Wl,-z,norelro
    -Wl,-z,noseparate-code
    -Wl,--build-id=none
)

target_link_libraries(audio-skid
    log
    aaudio
    opus
)