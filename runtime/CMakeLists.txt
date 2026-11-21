# tdort - runtime linked into every converted game.
# Converted games either link a copy prebuilt once into the converter's cache
# (built from this file as its own project), or add this directory with
# add_subdirectory() and link tdort.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(tdort CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
add_library(tdort STATIC
    src/runtime.cpp
    src/hle_kernel.cpp
    src/hle_file.cpp
    src/hle_graphics.cpp
    src/hle_intl.cpp
    src/hle_compress.cpp
    src/hle_jstring.cpp
    src/hle_report.cpp
    src/modules_default.cpp
    src/hle_input.cpp
    src/hle_tasks.cpp
    src/gfx.cpp
    src/audio.cpp
    src/hle_audio.cpp
    src/savestate.cpp
    src/platform_null.cpp
    src/platform_win32.cpp
)
target_include_directories(tdort PUBLIC include)
target_compile_features(tdort PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(tdort PUBLIC xinput winmm)
endif()

# main() lives in an object library so the game executable owns it.
add_library(tdort_main OBJECT src/main.cpp)
target_link_libraries(tdort_main PUBLIC tdort)
