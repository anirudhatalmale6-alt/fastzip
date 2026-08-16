@echo off
REM One-shot Windows build. Works with either Visual Studio or MinGW-w64.
REM Requires CMake on PATH (https://cmake.org/download/).
setlocal
cd /d "%~dp0"
if exist build rmdir /s /q build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto fail
cmake --build build --config Release
if errorlevel 1 goto fail
if exist build\Release\fastzip.exe copy /y build\Release\fastzip.exe build\fastzip.exe >nul
echo.
echo Built: %CD%\build\fastzip.exe
build\fastzip.exe --help
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
