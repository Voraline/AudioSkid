cmake_minimum_required(VERSION 3.10)
project(AudioSkidServer)

if(NOT MSVC)
    message(FATAL_ERROR "This project only supports MSVC.")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "/O2 /arch:AVX2 /fp:precise")

add_executable(AudioSkidServer main.cpp)

target_link_libraries(AudioSkidServer
    ws2_32
    ole32
    mmdevapi
    avrt
)
