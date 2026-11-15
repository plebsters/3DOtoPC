@echo off
rem Double-click to install Visual Studio C++ Build Tools (+ CMake tools) and the .NET 8 SDK.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Prerequisites.ps1" %*
