cmake_minimum_required(VERSION 3.16)
project(sm83 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings
if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Main executable
add_executable(sm83 src/main.cpp)
target_include_directories(sm83 PRIVATE include)

# Tests executable
add_executable(sm83_tests tests.cpp)
target_include_directories(sm83_tests PRIVATE include)

enable_testing()
add_test(NAME sm83_tests COMMAND sm83_tests)
