cmake_minimum_required(VERSION 3.28)
project(cpp23_hello LANGUAGES CXX)

# Require C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use clang++ if available
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(_CLANGXX clang++)
    if(_CLANGXX)
        set(CMAKE_CXX_COMPILER "${_CLANGXX}" CACHE FILEPATH "C++ compiler")
        message(STATUS "Using clang++: ${_CLANGXX}")
    endif()
endif()

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executable target
add_executable(hello src/main.cpp)

# Enable all warnings
target_compile_options(hello PRIVATE -Wall -Wextra -Wpedantic)
