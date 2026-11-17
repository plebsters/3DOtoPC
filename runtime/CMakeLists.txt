# tdort - runtime linked into every converted game.
# Converted games add this directory with add_subdirectory() and link tdort.
add_library(tdort STATIC
    src/runtime.cpp
    src/hle_kernel.cpp
    src/hle_file.cpp
    src/hle_graphics.cpp
    src/hle_intl.cpp
    src/hle_compress.cpp
    src/hle_jstring.cpp
    src/hle_report.cpp
    src/hle_input.cpp
    src/hle_tasks.cpp
    src/gfx.cpp
    src/audio.cpp
    src/hle_audio.cpp
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
