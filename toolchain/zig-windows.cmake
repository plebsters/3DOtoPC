# zig-windows.cmake - CMake toolchain file for the compiler bundled with 3DO to PC.
#
# toolchain\zig is zig (clang with the MinGW-w64 headers and libraries for
# Windows), toolchain\cmake and toolchain\ninja are portable CMake and Ninja.
# Nothing from Visual Studio or the Windows SDK is used, and the programs built
# need only DLLs that come with Windows 10 and 11. The zig-*.exe front ends
# (zigwrap.cpp) run "zig.exe <tool> ...".
get_filename_component(_tdo_tc "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(CMAKE_C_COMPILER   "${_tdo_tc}/zig/zig-cc.exe")
set(CMAKE_CXX_COMPILER "${_tdo_tc}/zig/zig-c++.exe")
set(CMAKE_C_COMPILER_TARGET   x86_64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-windows-gnu)
set(CMAKE_AR     "${_tdo_tc}/zig/zig-ar.exe"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_tdo_tc}/zig/zig-ranlib.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_tdo_tc}/zig/zig-rc.exe")
