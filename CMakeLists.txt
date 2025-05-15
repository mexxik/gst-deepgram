cmake_minimum_required(VERSION 3.10)
project(gst-deepgram-plugin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(src/plugins)
add_subdirectory(src/apps/transcribe-basic)

