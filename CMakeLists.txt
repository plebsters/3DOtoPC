cmake_minimum_required(VERSION 3.16)
project(3DOtoPC CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The C++ runtime is linked into tdocore.dll and the CLI, so they run on a PC
# without the Visual C++ Redistributable (no Visual Studio needed to use them).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(TDO_BUILD_TESTS "Build tests" ON)

add_subdirectory(runtime)

set(TDO_CORE_SOURCES
    core/src/opera_fs.cpp
    core/src/disasm.cpp
    core/src/analysis.cpp
    core/src/emit_cpp.cpp
    core/src/toolchain.cpp
    core/src/pipeline.cpp)

# Static core for the CLI and tests.
add_library(tdocore STATIC ${TDO_CORE_SOURCES})
target_include_directories(tdocore PUBLIC core/include runtime/include)

# DLL for the WPF GUI (P/Invoke).
add_library(tdocore_dll SHARED ${TDO_CORE_SOURCES})
target_include_directories(tdocore_dll PUBLIC core/include runtime/include)
target_compile_definitions(tdocore_dll PRIVATE TDO_BUILD_DLL)
# The DLL keeps the name tdocore.dll; its MSVC import library gets its own
# name so it does not collide with the static tdocore.lib.
set_target_properties(tdocore_dll PROPERTIES OUTPUT_NAME tdocore ARCHIVE_OUTPUT_NAME tdocore_import)

add_executable(3DOtoPC_cli cli/3do_to_pc_cli.cpp)
if(WIN32)
    enable_language(RC)
    target_sources(3DOtoPC_cli PRIVATE cli/cli.rc)   # the 3DO2PC icon on the .exe
endif()
set_target_properties(3DOtoPC_cli PROPERTIES OUTPUT_NAME "3DO to PC CLI")
target_link_libraries(3DOtoPC_cli PRIVATE tdocore)

# Ship runtime sources next to the tools so converted games can be built.
add_custom_command(TARGET 3DOtoPC_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/runtime "$<TARGET_FILE_DIR:3DOtoPC_cli>/runtime")

if(TDO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
