cmake_minimum_required(VERSION 3.10)
project(AudioSkidServer)

# Vendored Opus declares an old minimum CMake version; this keeps it
# configurable under modern CMake without touching Opus's own files.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(NOT MSVC)
    message(FATAL_ERROR "This project only supports MSVC.")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "/O2 /arch:AVX2 /fp:precise")

# Vendored Opus codec, built as a static lib.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus opus_build)

add_executable(AudioSkidServer main.cpp)

target_include_directories(AudioSkidServer PRIVATE
    third_party/opus/include
)

target_link_libraries(AudioSkidServer
    ws2_32
    ole32
    mmdevapi
    avrt
    opus
)
