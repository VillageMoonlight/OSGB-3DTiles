@echo off
setlocal
cd /d "C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles"

:: 激活 MSVC x64 环境
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

echo.
echo ======================================
echo Building osgb2tiles target...
echo ======================================
cd build
nmake osgb2tiles
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo ======================================
echo BUILD SUCCESS: build\bin\osgb2tiles.exe
echo ======================================
