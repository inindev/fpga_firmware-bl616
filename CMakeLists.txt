cmake_minimum_required(VERSION 3.15)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDK components are selected in defconfig

if(TANG_BOARD STREQUAL "mega60k")
    add_definitions(-DTANG_MEGA60K)
elseif(TANG_BOARD STREQUAL "mega138k")
    add_definitions(-DTANG_MEGA138K)
elseif(TANG_BOARD STREQUAL "console60k") 
    add_definitions(-DTANG_CONSOLE60K)
elseif(TANG_BOARD STREQUAL "console138k")
    add_definitions(-DTANG_CONSOLE138K)
elseif(TANG_BOARD STREQUAL "primer25k")
    add_definitions(-DTANG_PRIMER25K)
elseif(TANG_BOARD STREQUAL "nano20k")
    add_definitions(-DTANG_NANO20K)
endif()

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Add include directories
sdk_add_include_directories(
    . usb fpga ui core utils
)

# Add source files
file(GLOB_RECURSE MY_SOURCES
    "usb/*.cpp"
    "usb/*.c"
    "fpga/*.cpp"
    "ui/*.cpp"
    "core/*.cpp"
    "utils/*.cpp"
)

target_sources(app PRIVATE ${MY_SOURCES})
sdk_set_main_file(main.cpp)

# fw_version.h (FW_VERSION for the menu footer), regenerated on every build
set(FW_VERSION_H ${CMAKE_BINARY_DIR}/generated/fw_version.h)
add_custom_target(fw_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${FW_VERSION_H}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/version.cmake
    BYPRODUCTS ${FW_VERSION_H})
add_dependencies(app fw_version)
target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/generated)

project(tangcore CXX)

