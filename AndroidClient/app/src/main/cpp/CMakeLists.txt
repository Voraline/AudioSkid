cmake_minimum_required(VERSION 3.22.1)
project("audio-skid")

set(CMAKE_BUILD_TYPE Release)

# Vendored Opus declares an old minimum CMake version; this keeps it
# configurable under modern CMake without touching Opus's own files.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Vendored Opus codec, built as a static lib and linked into audio-skid.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
# arm64-v8a always has NEON, so skip Opus's runtime CPU-detection (RTCD) path
# entirely. OPUS_MAY_HAVE_NEON (default ON) is what pulls in the RTCD
# assembly sources whose celt_pitch_xcorr_neon symbol isn't actually built by
# this CMake config, causing an undefined-symbol link error. Turning it off
# (and presuming NEON instead) routes straight to the NEON-intrinsics C code,
# which does build correctly.
set(OPUS_MAY_HAVE_NEON OFF CACHE BOOL "" FORCE)
set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus opus_build)

add_library(audio-skid SHARED
    native-lib.cpp
)

target_compile_options(audio-skid PRIVATE
    -std=c++17
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
    -fno-exceptions
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

target_include_directories(audio-skid PRIVATE
    third_party/opus/include
)

target_link_libraries(audio-skid
    log
    aaudio
    opus
)
