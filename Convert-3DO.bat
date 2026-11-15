@echo off
rem Drag a 3DO disc image (.iso / .bin / .cue) or a folder of game files onto this file.
rem Everything runs on this PC: build tools, convert, compile, run, collect the log.
title 3DO to PC
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\convert.ps1" %*
