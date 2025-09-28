cmake_minimum_required(VERSION 3.18)
project(hft-programs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_CXX_FLAGS "-pg") - use only with GCC 's own profiler
cmake_policy(SET CMP0167 NEW)
# Fetch Catch2
include(FetchContent)
FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3 # or latest stable tag
)
FetchContent_MakeAvailable(Catch2)

# BOOST and LLVM do not ship .pc file yet so fall back to find_package()
find_package(Boost REQUIRED COMPONENTS system)
find_package(LLVM REQUIRED CONFIG)
find_package(LLVM REQUIRED Core)

# this is a generic tool adopted by modern lib developers to ease the build process.
find_package(PkgConfig REQUIRED)

# Use pkg-config to find the DPDK library
pkg_check_modules(DPDK REQUIRED libdpdk)

SET(_SOURCES_
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_main.cpp
)

SET(_HEADER_
    ${CMAKE_CURRENT_SOURCE_DIR}/hdr/io_uring_test_no_zero_copy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hdr/io_uring_test_zero_copy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hdr/dpdk-tbt-handler.h
)

######################
#Include Definitions #
######################
add_definitions(-DUSING_LOCK_FREE_CODE)
add_definitions(${LLVM_DEFINITIONS})

######################
#Include Directories #
######################
include_directories(
    ${Boost_INCLUDE_DIRS}
    /usr/include/llvm-12/
    /usr/include/llvm-c-12/
    ${CMAKE_CURRENT_SOURCE_DIR}/hdr
    ${LLVM_INCLUDE_DIRS}
    ${DPDK_INCLUDE_DIRS}
)

######################
#link Directories    #
######################
link_directories(

)

######################
# Create executable  #
######################

add_executable(${PROJECT_NAME} ${_SOURCES_} ${_HEADER_})

#set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
#set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")

llvm_map_components_to_libnames(llvm_libs core support)

target_compile_options(${PROJECT_NAME} PUBLIC -mssse3)

target_link_libraries(${PROJECT_NAME}
        -latomic -pthread
        ${Boost_LIBRARIES}
        -lpthread -Wl,--no-as-needed
        -lprofiler -ltcmalloc
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
        ${llvm_libs}
        ${DPDK_LIBRARIES}
        LLVMSupport
        Catch2::Catch2WithMain
        -luring
)

# Enable test discovery with CTest
include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})
