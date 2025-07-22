cmake_minimum_required(VERSION 3.10)
project(hft-programs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_CXX_FLAGS "-pg") - use only with GCC 's own profiler

# Fetch Catch2
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.3 # or latest stable tag
)
FetchContent_MakeAvailable(Catch2)

find_package(Boost REQUIRED COMPONENTS system)
find_package(LLVM REQUIRED CONFIG)
find_package(LLVM REQUIRED Core)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

SET( _SOURCES_
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_main.cpp
)

SET( _HEADER_
    ${CMAKE_CURRENT_SOURCE_DIR}/hdr/io_uring_test.h
)

######################
#Include Definitions #
######################
add_definitions(-DUSING_LOCK_FREE_CODE)

######################
#Include Directories #
######################
include_directories(
    ${Boost_INCLUDE_DIRS}
    /usr/include/llvm-12/
    /usr/include/llvm-c-12/
    ${CMAKE_CURRENT_SOURCE_DIR}/hdr
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

target_link_libraries(${PROJECT_NAME}
        -latomic -pthread
        ${Boost_LIBRARIES}
        -lpthread -Wl,--no-as-needed
        -lprofiler -ltcmalloc
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
        ${llvm_libs}
        LLVMSupport
        Catch2::Catch2WithMain
)

# Enable test discovery with CTest
include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})
