cmake_minimum_required(VERSION 2.8.8)

project(FileTools C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pipe")
set(CMAKE_EXE_LINKER_FLAGS "-fwhole-program -Wl,-z,norelro -Wl,--strip-all")

add_executable(joinfiles src/joinfiles.c)
