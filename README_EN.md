# OSGB → 3D Tiles Converter

> A complete toolchain for converting oblique photogrammetry OSGB data to Cesium 3D Tiles format, including a multi-threaded CLI engine, a native OSGB 3D viewer, and a Python GUI frontend.

**v0.0.1** · [中文文档](README.md) · [MIT License](LICENSE)

<p align="center">
  <img src="osgb2tiles_gui_preview.png" alt="OSGB-3DTiles Logo" width="200">
</p>

---

## Table of Contents

- [Features](#features)
- [Texture Format Benchmark](#texture-format-benchmark)
- [Architecture](#architecture)
- [Quick Start](#quick-start)
- [Build Instructions](#build-instructions)
- [Dependencies](#dependencies)
- [Known Issues & Solutions](#known-issues--solutions)
- [Output Structure](#output-structure)
- [Related Documentation](#related-documentation)
- [License](#license)

---

## Features

### 🔧 CLI Engine (`osgb2tiles.exe`)

| Feature | Description |
|---------|-------------|
| **Multi-threaded conversion** | Uses all CPU cores by default; each Tile block is processed independently |
| **Auto coordinate detection** | Reads metadata.xml (ContextCapture/Smart3D), production_meta.xml (DJI Terra), doc.xml (Metashape) |
| **Universal projection support** | GDAL/PROJ converts any CRS (CGCS2000, UTM, local systems) to WGS84 automatically |
| **Multiple texture formats** | JPEG / PNG / WebP / **KTX2 (Basis Universal ETC1S/UASTC)** |
| **Mesh simplification** | meshoptimizer-based with configurable ratio |
| **Draco compression** | Lossless geometry compression (~70% size reduction) |
| **Bursa-Wolf 7-parameter** | Optional local CRS fine-tuning for survey control-point alignment |
| **LOD tree generation** | Automatically generates multi-LOD tileset.json with decreasing geometricError |
| **Output formats** | B3DM (Cesium standard) or GLB (standalone files) |

### 🏔 OSGB 3D Viewer (`osgb_viewer.exe`)

A native OSGB data viewer built on OpenSceneGraph — browse oblique photogrammetry data directly without conversion:

| Feature | Description |
|---------|-------------|
| **PagedLOD streaming** | Supports large-scale oblique photogrammetry with automatic LOD scheduling |
| **Skybox background** | Built-in mountain skybox texture for immersive 3D browsing |
| **Fullscreen mode** | F11 toggle for fullscreen viewing |
| **Ortho export** | One-click orthophoto export (GeoTIFF format) |
| **Panorama export** | High-resolution panorama export (12K+) |
| **Native Win32 window** | No external dependencies, native Windows GUI |

### 🖥 GUI Frontend (`osgb2tiles_gui.py` / `osgb2tiles_gui.exe`)

| Feature | Description |
|---------|-------------|
| **Visual configuration** | Save/load JSON config files with all parameters |
| **Real-time log display** | Progress bar and conversion log (non-blocking main thread) |
| **OSGB preview** | Two modes: native OSG viewer (recommended) or Three.js WebGL preview |
| **3D Tiles preview** | Built-in Cesium local preview with rendering performance controls (SSE/memory/requests/globe error) |
| **Dark theme** | Catppuccin Mocha-style dark interface |
| **Standalone packaging** | PyInstaller-packaged exe, no Python required |

---

## Texture Format Benchmark

> **Test environment**: 8-core CPU, ~**330 MB** original OSGB data (ContextCapture oblique photogrammetry), default thread count (= CPU cores)

| Format | Output Size | Time | Ratio¹ | GPU-ready | Use Case |
|--------|-------------|------|--------|-----------|----------|
| **JPEG** (q=75) | ~350 MB | **~50 s** | 1.06× | ❌ | Quick preview, debugging |
| **WebP** (q=75) | **190 MB** | ~200 s | ~0.58× | ❌ | ⭐ **Recommended**: smallest size with good speed |
| **ETC1S** (KTX2) | **130 MB** | ~2000 s | ~0.39× | ✅ | Maximum compression, offline distribution |
| **UASTC+zstd** (KTX2) | ~600 MB² | ~200 s | ~1.8× | ✅ | High-quality GPU texture streaming |
| **PNG** | ~900 MB | ~60 s | ~2.7× | ❌ | Lossless, debugging only |

¹ Ratio = output size / original OSGB size (<1 means smaller than original)
² UASTC without zstd is ~1.4 GB; with zstd level-9 super-compression ~600 MB

### Recommendations

```
Fastest speed   →  JPEG  (50s, ideal for development)
Smallest size   →  ETC1S (130MB, accept 33-minute wait)
Speed + size    →  WebP  (190MB / 200s, recommended)
GPU performance →  UASTC+zstd (zero GPU decode overhead, best Cesium streaming)
```

---

## Architecture

### Processing Pipeline

```
OSGB Directory
   │
   ├─ 1. OsgbMetaReader   Parse metadata XML → extract coordinates → GDAL/PROJ → WGS84
   │
   ├─ 2. OsgbReader       Scan Tile_xxx blocks → OSG loads .osgb/.osg files
   │                      Extract geometry (vertices/normals/UVs) & textures
   │
   ├─ 3. GeometryConverter ENU coordinates → ECEF absolute coordinates
   │                       Optional Bursa-Wolf 7-parameter transform
   │
   ├─ 4. TextureProcessor  Resize textures (stb_image_resize)
   │                       Encode to target format (JPEG/PNG/WebP/KTX2)
   │
   ├─ 5. MeshSimplifier   (Optional) meshoptimizer mesh simplification
   │
   ├─ 6. GlbWriter        Assemble GLB/B3DM binary:
   │                       GLTF JSON + BIN + textures → single file
   │
   └─ 7. TilesetBuilder   Generate root tileset.json:
                            Compute bounding regions/boxes, geometricError hierarchy
```

### Coordinate Transformation Strategy

1. Read SRS field from `metadata.xml`, convert via **GDAL/PROJ** to WGS84
2. Fall back to built-in **Gauss-Krüger** inverse projection if GDAL fails
3. Command-line `--lon/--lat/--alt` has highest priority and bypasses metadata

### KTX2 Basis Universal Encoding

KTX2 (ETC1S/UASTC) is a GPU-native compressed texture format that reduces B3DM size by 30–50% compared to JPEG:

```
RGB pixel data
    → Expand RGB → RGBA (alpha=255; BasisU requires 4 channels)
    → ktxTexture2_Create (VK_FORMAT_R8G8B8A8_UNORM, generateMipmaps=FALSE)
    → ktxTexture_SetImageFromMemory
    → ktxTexture2_CompressBasisEx
    → ktxTexture_WriteToMemory
    → Write to GLB texture buffer
```

---

## Quick Start

### Option 1: GUI (Recommended)

```bat
python osgb2tiles_gui.py
:: Or run the packaged executable:
osgb2tiles_gui.exe
```

### Option 2: Command Line

```bat
:: Minimal usage (auto-reads metadata.xml; default KTX2 textures)
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles

:: Manual coordinates + 4 threads + JPEG textures
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles ^
  --lon 120.266674 --lat 36.260571 --alt 0 ^
  --threads 4 --tex-format jpg --jpeg-quality 80

:: KTX2 high-quality (UASTC) + Draco geometry compression
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles ^
  --tex-format ktx2 --ktx2-mode uastc --ktx2-quality 4 ^
  --draco --draco-bits 14

:: Use a config file (includes 7-parameter transform, etc.)
osgb2tiles.exe -i D:\data\OSGB --config osgb2tiles_config.json
```

### Option 3: OSGB Viewer

```bat
:: Browse OSGB directory directly
osgb_viewer.exe D:\data\OSGB
```

### Parameter Reference

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-i/--input` | *required* | OSGB input directory |
| `-o/--output` | `./output` | 3D Tiles output directory |
| `--lon/--lat/--alt` | auto-detect | WGS84 origin coordinates |
| `--format` | `b3dm` | Output format: `b3dm` or `glb` |
| `--threads` | `4` | Parallel thread count (0 = auto) |
| `--tex-format` | `ktx2` | Texture format: `ktx2`/`jpg`/`png`/`webp` |
| `--ktx2-mode` | `etc1s` | KTX2 mode: `etc1s` (smaller) or `uastc` (higher quality) |
| `--ktx2-quality` | `2` | KTX2 quality level [1-5] |
| `--jpeg-quality` | `85` | JPEG quality [1-100] |
| `--webp-quality` | `80` | WebP lossy quality [1-100] |
| `--simplify` | false | Enable mesh simplification |
| `--simplify-ratio` | `0.5` | Simplification ratio [0.1–1.0] |
| `--tex-size` | `2048` | Max texture dimension (pixels) |
| `--draco` | false | Enable Draco geometry compression |
| `--draco-bits` | `14` | Draco quantization bits [8-16] |
| `--geo-error` | `0.5` | geometricError scale factor [0.1-2.0] |
| `--config` | — | JSON config file path |
| `-v/--verbose` | false | Verbose logging |

> Full parameter list: [CLI Parameters](README_CLI参数说明.md) · Config file fields: [Config Reference](README_配置文件说明.md)

---

## Build Instructions

### Prerequisites

- **OS**: Windows 10/11 x64
- **Compiler**: Visual Studio 2019 Build Tools (MSVC v142)
- **vcpkg**: Installed at `C:\vcpkg` (manifest mode)
- **CMake**: 3.20+ (installed with Visual Studio)

### Steps

#### 1. Initial Configuration (first time only)

```cmd
cmd /c "call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && ^
  cmake -S . -B build ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_BUILD_TYPE=Release"
```

> First configuration auto-downloads and builds all dependencies via vcpkg (30–60 minutes depending on network).

#### 2. Incremental Build (after source changes)

```cmd
cmd /c "call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cd build && nmake /S"
```

#### 3. Build Output

```
build/bin/osgb2tiles.exe         ← Main converter
build/bin/osgb_viewer.exe        ← OSGB 3D viewer
build/bin/*.dll                  ← All dependency DLLs (90 files, fully self-contained)
build/bin/osgPlugins-3.6.5/     ← OSG reader plugins (91 files)
build/bin/proj/proj.db           ← PROJ coordinate database
build/bin/gdal-data/             ← GDAL projection data
```

### GUI Packaging (optional)

```bat
pip install pyinstaller
pyinstaller osgb2tiles_gui.spec
:: Output: dist/osgb2tiles_gui.exe
```

---

## Dependencies

| Library | Version | Purpose | Source |
|---------|---------|---------|--------|
| **OpenSceneGraph** | 3.6.5 | Read OSGB/OSG format files | vcpkg `osg` |
| **GDAL** | 3.9.x | Coordinate system conversion (any → WGS84) | vcpkg `gdal` |
| **PROJ** | 9.4.x | Projection database (GDAL dependency) | vcpkg via gdal |
| **nlohmann/json** | 3.11.x | JSON config parsing | vcpkg `nlohmann-json` |
| **cxxopts** | 3.1.x | CLI argument parsing | vcpkg `cxxopts` |
| **stb** | latest | Image load/resize/JPEG encoding | vcpkg `stb` |
| **libwebp** | 1.4.x | WebP texture encoding | vcpkg `libwebp` |
| **meshoptimizer** | 0.21.x | Mesh simplification & optimization | vcpkg `meshoptimizer` |
| **draco** | 1.5.x | Geometry Draco compression (optional) | vcpkg `draco` |
| **libktx** | **4.4.2** | KTX2/Basis Universal encoding | vcpkg `ktx` (overlay) |
| **tinygltf** | 2.9.x | GLTF JSON helpers | vcpkg `tinygltf` |
| Python | 3.8+ | GUI frontend runtime | System |
| PyInstaller | 6.x | Package GUI as standalone exe | pip |

---

## Known Issues & Solutions

<details>
<summary><b>KTX2 Compression Fails (CompressBasis failed: 10)</b></summary>

**Root cause**: In libktx v4.4.2, `generateMipmaps=KTX_TRUE` combined with `ktxTexture2_CompressBasisEx` triggers a compatibility bug.

**Fix**: Always set `ci.generateMipmaps = KTX_FALSE` — mipmaps are generated at runtime by the GPU renderer.

Additional pitfalls:
- BasisU requires RGBA (4-channel) data
- Use `VK_FORMAT_R8G8B8A8_UNORM`, not the SRGB variant
- Set `inputSwizzle` explicitly to `'r','g','b','a'`
</details>

<details>
<summary><b>OSGB File Load Failure (Windows 8.3 Short Path)</b></summary>

**Root cause**: Chinese characters or spaces in directory paths cause 8.3 short path conversion, breaking OSG plugin file resolution.

**Fix**: Call `GetLongPathNameW` at startup to restore full UTF-16 paths.
</details>

<details>
<summary><b>PROJ Cannot Find Database (Coordinate Conversion Fails)</b></summary>

**Fix**: At startup, the program automatically searches for `proj/proj.db` relative to the executable and sets `PROJ_DATA` — no manual configuration needed.
</details>

<details>
<summary><b>vcpkg ktx Library Requires an Overlay</b></summary>

The standard vcpkg `ktx` port excludes the Basis Universal encoder. Use the project's custom `vcpkg-overlay/ktx/` directory.
</details>

---

## Output Structure

```
<output_dir>/
  tileset.json              ← Root entry point (Cesium loads this file)
  Data/
    Tile_+007_+009/
      tileset.json          ← Per-block LOD tree
      Tile_+007_+009.b3dm   ← Lowest LOD (root node)
      Tile_+007_+009_L15_0u.b3dm
      ...                   ← L15~L22 LOD levels
    Tile_+007_+010/
      ...
```

### Cesium Integration

```javascript
const tileset = await Cesium.Cesium3DTileset.fromUrl(
    'http://localhost:8080/tileset.json'
);
viewer.scene.primitives.add(tileset);
```

Serve locally with a simple HTTP server:
```bat
npx http-server D:\output\3DTiles --cors -p 8080
```

---

## Related Documentation

| Document | Content |
|----------|---------|
| [README_CLI参数说明.md](README_CLI参数说明.md) | Full CLI parameter reference |
| [README_配置文件说明.md](README_配置文件说明.md) | JSON config file field reference |
| [DEV_ENV.md](DEV_ENV.md) | Development environment paths & build commands |
| [README.md](README.md) | 中文文档 |

---

## License

This project is licensed under the [MIT License](LICENSE).
