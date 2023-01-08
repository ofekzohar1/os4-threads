cmake_minimum_required(VERSION 3.16)
project(os4 C)

set(CMAKE_C_STANDARD 11)

add_executable(os4 pfind.c)
target_link_libraries(os4 pthread)
