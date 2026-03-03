cmake_minimum_required(VERSION 3.27)
project(chess_engine)

set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_executable(chess_magic main.cpp uci.cpp
        magicBoard.cpp
        search.cpp
        staticEvals.cpp
        hash.cpp
        transposition.cpp
        staticEvals.h
)

add_executable(test_board test/test_board.cpp staticEvals.cpp)
