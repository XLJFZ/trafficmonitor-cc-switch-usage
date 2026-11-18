@echo off
setlocal
if "%~1"=="x64" goto build
if "%~1"=="x86" goto build
echo Usage: build.cmd x64 or build.cmd x86
exit /b 1
:build
pushd "%~dp0"
if not exist "%~1" mkdir "%~1"
rc /nologo /fo "%~1\options.res" options.rc
if errorlevel 1 (popd & exit /b 1)
cl /nologo /LD /std:c++17 /utf-8 /EHsc /O2 /MT /D_CRT_SECURE_NO_WARNINGS /Fo"%~1\CCSwitchUsage.obj" CCSwitchUsage.cpp "%~1\options.res" /link /OUT:"%~1\CCSwitchUsage.dll" /IMPLIB:"%~1\CCSwitchUsage.lib" winsqlite3.lib user32.lib gdi32.lib comctl32.lib
set "result=%errorlevel%"
popd
exit /b %result%
