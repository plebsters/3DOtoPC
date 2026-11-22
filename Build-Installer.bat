@echo off
rem Build-Installer.bat - builds the app (Build-App.bat) and packs it into a Windows installer:
rem   installer\Output\3DO2PC-<version>-Setup.exe
rem Requires what Build-App.bat needs (Build Tools for Visual Studio 2022, .NET 8 SDK) plus
rem Inno Setup 6 (free): https://jrsoftware.org/isdl.php  or  winget install JRSoftware.InnoSetup
rem Usage: Build-Installer.bat [version]      (default version: today's date, e.g. 2026.11.21)
setlocal EnableExtensions
title Build the 3DO to PC installer
cd /d "%~dp0"

set "VERSION=%~1"
if not defined VERSION for /f "usebackq tokens=*" %%v in (`powershell -NoProfile -Command "Get-Date -Format yyyy.M.d"`) do set "VERSION=%%v"
echo Version: %VERSION%

rem ---------------------------------------------------------------- Inno Setup
set "ISCC="
for %%p in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if not defined ISCC if exist "%%~p" set "ISCC=%%~p"
if not defined ISCC for /f "tokens=*" %%i in ('where iscc 2^>nul') do if not defined ISCC set "ISCC=%%i"
if not defined ISCC goto :noinno
echo Using Inno Setup: %ISCC%

rem ---------------------------------------------------------------- app
echo.
echo [1/2] Building the app (Build-App.bat)...
call "%~dp0Build-App.bat" /nopause
if errorlevel 1 goto :fail
if not exist "dist\3DO to PC.exe" goto :nogui
if not exist "dist\3DO Launcher.exe" goto :nogui

rem ---------------------------------------------------------------- installer
echo.
echo [2/2] Packing the installer...
"%ISCC%" /Q /DMyAppVersion=%VERSION% "installer\3DO2PC.iss"
if errorlevel 1 goto :fail
echo.
echo Done: installer\Output\3DO2PC-%VERSION%-Setup.exe
start "" explorer "%~dp0installer\Output"
echo.
pause
exit /b 0

:noinno
echo Inno Setup 6 was not found. It is free and makes the installer:
echo   https://jrsoftware.org/isdl.php
where winget >nul 2>nul
if errorlevel 1 goto :end
echo.
choice /c YN /m "Install it now with winget"
if errorlevel 2 goto :end
winget install --id JRSoftware.InnoSetup -e --accept-source-agreements --accept-package-agreements
echo.
echo Run Build-Installer.bat again when the install has finished.
goto :end

:nogui
echo.
echo ERROR: the Windows apps were not built (dist\"3DO to PC.exe" is missing).
echo The installer needs them: install the .NET 8 SDK (Install-Prerequisites.bat does it) and run this again.
goto :end

:fail
echo.
echo ERROR: the build failed. Copy the text in this window and send it to me.

:end
echo.
pause
exit /b 1
