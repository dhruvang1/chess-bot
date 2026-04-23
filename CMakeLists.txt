cmake_minimum_required(VERSION 3.27)
project(chess_engine)

set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(chess_magic main.cpp staticEvals.cpp)

add_executable(test_board test/test_board.cpp staticEvals.cpp)
add_executable(test_nnue test/test_nnue.cpp)
