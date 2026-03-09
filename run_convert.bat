@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set VCPKG_BIN=C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles\build\vcpkg_installed\x64-windows\bin
set OSG_PLUGINS=C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles\build\vcpkg_installed\x64-windows\plugins

set PATH=%VCPKG_BIN%;%PATH%
set OSG_LIBRARY_PATH=%OSG_PLUGINS%
set OSG_PLUGIN_PATH=%OSG_PLUGINS%\osgPlugins-3.6.5
set PROJ_DATA=C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles\build\vcpkg_installed\x64-windows\share\proj

echo === osgb2tiles 转换启动 ===
echo OSG_PLUGIN_PATH=%OSG_PLUGIN_PATH%

build\bin\osgb2tiles.exe -i "C:\Users\hubiao\Desktop\AIStudio\geonexus-proxy\data_examples\OSGB" -o "C:\Users\hubiao\Desktop\AIStudio\geonexus-proxy\data_examples\OSGB-3DTiles" --format b3dm --threads 4
echo 退出码: %ERRORLEVEL%
