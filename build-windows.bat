@echo off
setlocal EnableExtensions

set "BUILD_DIR=build-windows"
set "DIST_DIR=dist"

where ninja >nul 2>nul
if errorlevel 1 (
    echo ERROR: ninja was not found in PATH.
    echo Run this from a Qt command prompt or add Ninja to PATH.
    exit /b 1
)

where qt-cmake >nul 2>nul
if errorlevel 1 (
    echo qt-cmake not found; trying plain CMake.
    echo If Qt is not discovered automatically, set CMAKE_PREFIX_PATH to your Qt kit.
    cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
) else (
    qt-cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 exit /b 1

if not exist "%BUILD_DIR%\term0.exe" (
    echo ERROR: build succeeded but "%BUILD_DIR%\term0.exe" was not found.
    exit /b 1
)

echo Built: %BUILD_DIR%\term0.exe

where windeployqt >nul 2>nul
if errorlevel 1 (
    echo windeployqt not found; skipping portable deployment folder.
    echo Run windeployqt on %BUILD_DIR%\term0.exe before copying it to another PC.
    exit /b 0
)

if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"
copy /y "%BUILD_DIR%\term0.exe" "%DIST_DIR%\term0.exe" >nul
windeployqt --release --no-translations "%DIST_DIR%\term0.exe"
if errorlevel 1 exit /b 1

echo Portable build: %DIST_DIR%\term0.exe
endlocal
