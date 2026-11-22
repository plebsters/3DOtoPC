@echo off
rem Build-App.bat - builds 3DO to PC into dist\
rem   dist\"3DO to PC.exe" - the Windows app (needs the .NET 8 SDK to build)
rem   dist\"3DO to PC CLI.exe" - command-line converter
rem   dist\"3DO Launcher.exe" - lists your converted games and starts them
rem   dist\tdocore.dll, dist\runtime\  - used by both to build games
rem Requires: Build Tools for Visual Studio 2022 with "Desktop development with C++".
rem Build-App.bat /nopause : no pause and no Explorer window at the end (Build-Installer.bat uses it).
setlocal EnableExtensions
title Build 3DO to PC
cd /d "%~dp0"
set "NOPAUSE="
if /i "%~1"=="/nopause" set "NOPAUSE=1"

rem ---------------------------------------------------------------- compiler
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "VS="
rem Prefer an install that has both the C++ compiler and the CMake tools.
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VS=%%i"
if not defined VS for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto :novs
echo Using Visual Studio: %VS%
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :fail
where cl >nul 2>nul
if errorlevel 1 goto :novs

rem Find cmake.exe / ninja.exe: in any Visual Studio install, a standalone CMake, or PATH.
set "CMAKE_EXE="
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -all -products * -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`) do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE for /f "tokens=*" %%i in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
if defined CMAKE_EXE for %%d in ("%CMAKE_EXE%") do set "PATH=%%~dpd;%PATH%"
set "NINJA_EXE="
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -all -products * -find "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"`) do if not defined NINJA_EXE set "NINJA_EXE=%%i"
if defined NINJA_EXE for %%d in ("%NINJA_EXE%") do set "PATH=%%~dpd;%PATH%"
if defined CMAKE_EXE echo Using CMake: %CMAKE_EXE%
where cmake >nul 2>nul
if errorlevel 1 goto :nocmake
rem Ninja is preferred; NMake comes with the C++ tools and works too.
set "GEN=Ninja"
where ninja >nul 2>nul
if errorlevel 1 set "GEN=NMake Makefiles"
echo Using generator: %GEN%
rem Start fresh if an earlier build used a different generator.
if exist "build\CMakeCache.txt" findstr /c:"CMAKE_GENERATOR:INTERNAL=%GEN%" "build\CMakeCache.txt" >nul 2>nul
if exist "build\CMakeCache.txt" if errorlevel 1 rmdir /s /q build

rem ---------------------------------------------------------------- native core
echo.
echo [1/3] Building the converter core (C++)...
cmake -S . -B build -G "%GEN%" -DCMAKE_BUILD_TYPE=Release -DTDO_BUILD_TESTS=OFF
if errorlevel 1 goto :fail
rem Show "[ 42% 21/50]" before every compiled file (Ninja builds only).
set "NINJA_STATUS=[%%p %%f/%%t] "
cmake --build build --target 3DOtoPC_cli tdocore_dll
if errorlevel 1 goto :fail

set "DIST=%~dp0dist"
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"
copy /y "build\3DO to PC CLI.exe" "%DIST%\" >nul || goto :fail
copy /y "build\tdocore.dll" "%DIST%\" >nul || goto :fail
xcopy /e /i /q /y "runtime" "%DIST%\runtime" >nul || goto :fail

rem ---------------------------------------------------------------- GUI
echo.
echo [2/3] Building the Windows app (C# WPF)...
where dotnet >nul 2>nul
if errorlevel 1 goto :nodotnet
dotnet publish "gui\TdoConverter\TdoConverter.csproj" -c Release -r win-x64 --self-contained false -o "%DIST%"
if errorlevel 1 goto :fail
rem The app is named "3DO to PC" (the .NET launcher can be renamed freely).
if exist "%DIST%\3DO to PC.exe" del /q "%DIST%\3DO to PC.exe"
move /y "%DIST%\TdoConverter.exe" "%DIST%\3DO to PC.exe" >nul
rem The game launcher: lists the converted games in games\ and starts them.
dotnet publish "gui\Launcher\Launcher.csproj" -c Release -r win-x64 --self-contained false -o "%DIST%"
if errorlevel 1 goto :fail
set "GUI=yes"
goto :done

:nodotnet
echo   .NET 8 SDK not found, so the Windows app was skipped.
echo   Install it from https://dotnet.microsoft.com/download/dotnet/8.0 and run this again.
echo   The command-line converter was built and works on its own.
set "GUI=no"

:done
echo.
echo [3/3] Done. Output: %DIST%
if "%GUI%"=="yes" echo   "3DO to PC.exe"    - the Windows app
echo   "3DO to PC CLI.exe" - command line:  "3DO to PC CLI" convert game.iso outfolder
if defined NOPAUSE exit /b 0
start "" explorer "%DIST%"
echo.
pause
exit /b 0

:novs
echo ERROR: Visual Studio or Build Tools with C++ was not found.
echo Install "Build Tools for Visual Studio 2022" (free) and tick "Desktop development with C++":
echo   https://visualstudio.microsoft.com/downloads/  (under "Tools for Visual Studio")
goto :end

:nocmake
echo.
echo CMake was not found in any Visual Studio install, in %ProgramFiles%\CMake, or on PATH.
echo --- details (please send these lines to me if this keeps happening) ---
echo Visual Studio installs:
"%VSWHERE%" -all -products * -property installationPath
echo Installs reporting the CMake component:
"%VSWHERE%" -all -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
echo Contents of %VS%\Common7\IDE\CommonExtensions\Microsoft:
dir /b "%VS%\Common7\IDE\CommonExtensions\Microsoft" 2>nul
echo ---------------------------------------------------------------------
echo.
echo Install-Prerequisites.bat adds it to your Build Tools (asks for admin, a few minutes).
echo.
if defined NOPAUSE goto :end
choice /c YN /m "Run Install-Prerequisites.bat now"
if errorlevel 2 goto :end
start "" "%~dp0Install-Prerequisites.bat"
echo When the installer says "All set", run Build-App.bat again.
goto :end

:fail
echo.
echo ERROR: the build failed. Copy the text in this window and send it to me.

:end
echo.
if not defined NOPAUSE pause
exit /b 1
