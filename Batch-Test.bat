@echo off
rem Drag the folder that holds your 3DO disc images or game folders onto this file.
rem Each game is converted into games\ and test-run without a window; the results
rem go to batch-results\ and a zip to send back. Safe to stop and start again.
title 3DO to PC - batch test
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\batch-test.ps1" %*
