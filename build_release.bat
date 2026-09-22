@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found in PATH.
  echo Install Visual Studio 2022 with Desktop development with C++ and CMake tools.
  exit /b 1
)

cmake -S . -B build -A x64
if errorlevel 1 exit /b 1

cmake --build build --config Release
if errorlevel 1 exit /b 1

copy /Y "build\Release\AudioDuplicate.exe" ".\AudioDuplicate.exe" >nul
if errorlevel 1 exit /b 1

echo.
echo Built: %CD%\AudioDuplicate.exe
endlocal
