@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul || (echo CMake not found.& exit /b 1)
cmake -S . -B build -A x64
if errorlevel 1 exit /b %errorlevel%
cmake --build build --config Release
if errorlevel 1 exit /b %errorlevel%
echo.
echo Built: %CD%\build\Release\AudioDuplicate.exe
