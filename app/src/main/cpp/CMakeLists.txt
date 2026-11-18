cmake_minimum_required(VERSION 3.18.1)
project(noisecancel LANGUAGES CXX)

add_library(noisecancel SHARED anc_engine.cpp)
target_compile_options(noisecancel PRIVATE -O3 -Wall)
target_link_libraries(noisecancel aaudio log)
