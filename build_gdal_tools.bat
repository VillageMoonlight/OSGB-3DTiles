@echo off
setlocal
cd /d "C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles"

:: 激活 MSVC x64 环境
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set CMAKE_EXE=C:\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe

echo.
echo ======================================
echo Step 1: CMake Configure (with GDAL tools)
echo ======================================
cd build
"%CMAKE_EXE%" -S .. -B . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo CMAKE CONFIGURE FAILED
    exit /b 1
)

echo.
echo ======================================
echo Step 2: Build (nmake)
echo ======================================
nmake /S
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo ======================================
echo BUILD SUCCESS
echo ======================================
