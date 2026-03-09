# OSGB-3DTiles 开发环境速查

## 关键路径

| 项目 | 路径 |
|------|------|
| 项目根目录 | `C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles` |
| **编译输出 exe** | `build\bin\osgb2tiles.exe` |
| **自包含运行目录** | `build\bin\` （**完全自包含，无需系统环境变量**）|
| ↳ 所有 Release DLL | `build\bin\*.dll`（90 个，含 OSG/GDAL/WebP 等）|
| ↳ OSG 读取插件 | `build\bin\osgPlugins-3.6.5\`（91 个，含 osgdb_osg.dll 等）|
| ↳ PROJ 数据库 | `build\bin\proj\proj.db`（坐标转换数据库）|
| ↳ GDAL 数据 | `build\bin\gdal-data\`（169 个数据文件）|
| vcpkg 根目录 | `C:\vcpkg`（manifest 模式，依赖写在 `vcpkg.json`）|
| vcpkg 安装的 DLL 源 | `build\vcpkg_installed\x64-windows\bin\` |
| VS 编译器环境脚本 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |

---

## 重新编译（修改 C++ 源码后）

```cmd
:: 在项目根目录的 PowerShell 中执行：
cmd /c "call `"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat`" && cd build && nmake /S"
```

> **注意**：cmake 不在系统 PATH，但 build 目录已有 CMakeCache.txt，直接用 nmake 增量编译即可，无需重新 cmake configure。
> 若修改了 CMakeLists.txt 或 vcpkg.json（如新增依赖），需先重新 configure：
> ```cmd
> cmd /c "call `"...\vcvars64.bat`" && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release"
> ```

---

## 新增 vcpkg 依赖后的 DLL 部署

新增依赖并重编后，把新 DLL 也复制到 `build\bin\`：

```powershell
# 示例：复制所有 webp 相关 DLL
Get-ChildItem "build\vcpkg_installed\x64-windows\bin" -Filter "*.dll" |
  Where-Object { $_.Name -match "关键词" } |
  ForEach-Object { Copy-Item $_.FullName "build\bin\" -Force }
```

---

## 启动 GUI

```powershell
cd C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles
python osgb2tiles_gui.py
```

GUI 会自动使用 `build\bin\osgb2tiles.exe`（优先），找不到时用同目录的 exe。

---

## 常见错误排查

| 错误 | 原因 | 解决 |
|------|------|------|
| 退出码 `3221225781` (0xC0000135) | DLL 缺失 | 检查 `build\bin\` 是否有所有依赖 DLL |
| `cmake 不是内部命令` | cmake 不在 PATH | 用上述完整 `cmd /c "call vcvars64 && nmake"` 命令 |
| `vcpkg install` 报错 manifest mode | vcpkg 为 manifest 模式 | 修改 `vcpkg.json` 后直接跑 cmake configure，自动安装 |
| GUI 找不到 exe | exe 不在 `build\bin` 或同目录 | 确保编译后 exe 在 `build\bin\osgb2tiles.exe` |

---

## 源码结构

```
src/
├── Config.h              # 所有配置项（含 Transform7P 结构体）
├── main.cpp              # CLI 入口、loadJsonConfig、线程调度
├── GeometryConverter.cpp # ENU→ECEF + Bursa-Wolf 七参数变换
├── TextureProcessor.cpp  # JPEG / PNG / WebP 纹理编码
├── GlbWriter.cpp         # 写出 GLB / B3DM 文件
├── TilesetBuilder.cpp    # 生成 tileset.json
├── OsgbReader.cpp        # 读取 OSGB 几何/纹理
├── OsgbMetaReader.cpp    # 读取元数据坐标（GDAL/GK）
└── MeshSimplifier.cpp    # meshoptimizer 网格简化
```

---

## GUI 打包为单文件 EXE（PyInstaller）

```powershell
cd C:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles
pyinstaller --clean osgb2tiles_gui.spec
# 输出：dist\osgb2tiles_gui.exe（约 17 MB，含 Python 运行时 + pyproj）
# 打包完成后同步到 build\bin\：
Copy-Item "dist\osgb2tiles_gui.exe" "build\bin\osgb2tiles_gui.exe" -Force
Copy-Item "osgb2tiles_gui.ico"     "build\bin\osgb2tiles_gui.ico"  -Force
```

**说明**：
- `osgb2tiles_gui.spec` 已配置 pyproj + PROJ 数据库（七参数坐标转换必需）
- `console=False`：不显示控制台黑窗
- 排除了 matplotlib/numpy/torch 等无关大库，保持体积最小

---

## 打包 Windows 安装包（Inno Setup）

**ISCC.exe 路径**（用户级安装，不在 PATH，需写全路径）：

```
C:\Users\hubiao\AppData\Local\Programs\Inno Setup 6\ISCC.exe
```

**编译命令**（在项目根目录执行）：

```powershell
& "C:\Users\hubiao\AppData\Local\Programs\Inno Setup 6\ISCC.exe" "osgb2tiles_setup.iss"
# 输出：dist\OSGB2Tiles_v2.2_Setup.exe（约 70 MB）
```

**ISS 脚本**：`osgb2tiles_setup.iss`，打包来源：`build\bin\`（含 GUI/CLI exe、所有 DLL、vcredist）

---

## 完整发布流程（修改代码后）

| 步骤 | 命令 | 说明 |
|------|------|------|
| 1 编译 C++ | `cmd /c "build_main.bat"` | 更新 `build\bin\osgb2tiles.exe` |
| 2 打包 GUI | `pyinstaller --clean osgb2tiles_gui.spec` | 生成 `dist\osgb2tiles_gui.exe` |
| 3 同步 GUI | `Copy-Item dist\osgb2tiles_gui.exe build\bin\ -Force` | 同步到 build\bin |
| 4 打包安装包 | `& "...\ISCC.exe" osgb2tiles_setup.iss` | 生成 `dist\OSGB2Tiles_v2.2_Setup.exe` |
