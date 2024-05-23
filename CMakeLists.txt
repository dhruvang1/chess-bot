cmake_minimum_required(VERSION 3.27)
project(chess_engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_executable(chess_engine main.cpp uci.cpp
        board.cpp
        search.cpp
        staticEvals.cpp
)
