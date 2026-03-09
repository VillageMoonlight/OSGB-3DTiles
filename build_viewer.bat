@echo off
setlocal

:: 激活 MSVC x64 环境
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: cmake 由 vcvars 加入 PATH（VS 内置 cmake 路径）
:: 若未找到则从 vcpkg 工具目录搜索
where cmake >nul 2>&1
if errorlevel 1 (
    echo [build_viewer] cmake not in PATH, searching vcpkg tools...
    for /f "delims=" %%i in ('dir /s /b "C:\vcpkg\downloads\tools\cmake-*\bin\cmake.exe" 2^>nul') do (
        set CMAKE_EXE=%%i
        goto :found_cmake
    )
    echo [build_viewer] ERROR: cmake.exe not found. Please install cmake.
    exit /b 1
    :found_cmake
    echo [build_viewer] Found cmake: %CMAKE_EXE%
) else (
    set CMAKE_EXE=cmake
)

:: Step 1: cmake configure（重新生成 Makefile 含 osgb_viewer 目标）
echo.
echo ========================================
echo [1/2] cmake configure...
echo ========================================
%CMAKE_EXE% -S . -B build ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [build_viewer] cmake configure FAILED.
    exit /b 1
)

:: Step 2: nmake 仅编译 osgb_viewer 目标
echo.
echo ========================================
echo [2/2] nmake osgb_viewer...
echo ========================================
cd build
nmake osgb_viewer
if errorlevel 1 (
    echo [build_viewer] nmake FAILED.
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCESS: build\bin\osgb_viewer.exe
echo ========================================
