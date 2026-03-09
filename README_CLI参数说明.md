# osgb2tiles 命令行参数说明

`osgb2tiles.exe` 是 OSGB 倾斜摄影数据转 3D Tiles 的核心转换引擎，支持任意投影坐标系（需配置 GDAL/PROJ 环境）。

---

## 基本用法

```bat
osgb2tiles.exe -i <OSGB目录> -o <输出目录> [选项]
```

---

## 参数列表

### 路径与坐标

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--input` | `-i` | 路径 | *必填* | OSGB 数据输入目录（含 Data/ 或直接含 Tile_xxx/） |
| `--output` | `-o` | 路径 | `./output` | 3D Tiles 输出目录（自动创建） |
| `--config` | — | 路径 | — | JSON 配置文件路径，可包含全部参数（含七参数） |
| `--lon` | — | 浮点 | 自动检测 | 原点经度（WGS84，十进制度）。不填则从 metadata.xml 自动读取 |
| `--lat` | — | 浮点 | 自动检测 | 原点纬度（WGS84，十进制度） |
| `--alt` | — | 浮点 | 自动检测 | 原点椭球高（米） |

### 输出格式

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--format` | 字符串 | `b3dm` | 输出格式：`b3dm`（Cesium 3D Tiles）或 `glb` |
| `--threads` | 整数 | `4` | 并行转换线程数（0 = 自动使用所有核心） |

### 网格简化

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--simplify` | 开关 | false | 启用网格简化（减少三角面数） |
| `--simplify-ratio` | 浮点 | `0.5` | 简化比例 `[0.1~1.0]`，1.0 = 不简化 |

### 纹理格式

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--tex-format` | 字符串 | `ktx2` | 纹理格式：`ktx2` / `jpg` / `png` / `webp` |
| `--compress-tex` | 开关 | false | 启用纹理格式转换（配合 tex-format 使用） |
| `--tex-size` | 整数 | `2048` | 纹理最大尺寸（像素），超过则降采样 |

**KTX2 参数**（`--tex-format ktx2` 时有效）：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--ktx2-mode` | 字符串 | `etc1s` | 压缩模式：`etc1s`（小体积）或 `uastc`（高质量） |
| `--ktx2-quality` | 整数 | `2` | 压缩质量 `[1-5]`，越大压缩越慢、质量越好 |
| `--ktx2-mipmaps` | 布尔 | `true` | 是否生成 Mipmaps（强烈建议开启，减少远处闪烁） |

**JPEG 参数**（`--tex-format jpg` 时有效）：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--jpeg-quality` | 整数 | `85` | JPEG 压缩质量 `[1-100]` |

**WebP 参数**（`--tex-format webp` 时有效）：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--webp-quality` | 整数 | `80` | WebP 有损质量 `[1-100]` |
| `--webp-lossless` | 布尔 | false | 启用 WebP 无损模式 |

### 几何压缩

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--draco` | 开关 | false | 启用 Draco 几何压缩（体积可减约 70%） |
| `--draco-bits` | 整数 | `14` | Draco 位置量化位数 `[8-16]`，越小压缩率越高 |

### LOD 控制

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--geo-error` | 浮点 | `0.5` | geometricError 比例系数 `[0.1~2.0]`。越大表示 LOD 切换越激进，加载更快但细节过早出现 |

### 其他

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--verbose` | `-v` | 开关 | false | 打印详细调试日志 |
| `--help` | `-h` | — | — | 显示帮助信息 |

---

## 坐标原点说明

程序**自动读取坐标**的优先级：

1. 命令行 `--lon/--lat/--alt`（最高优先级）  
2. `<输入目录>/metadata.xml`（ContextCapture/Smart3D 格式）  
3. `<输入目录>/production_meta.xml`（大疆智图格式）  
4. `<输入目录>/doc.xml`（Agisoft Metashape 格式）

元数据中的 SRS 通过 **GDAL/PROJ** 自动转换为 WGS84。  
支持 CGCS2000、UTM、地方独立坐标系等一切 PROJ 可解析的投影系统。

> ⚠ 若无元数据且未手动指定坐标，程序将退出并提示错误。

---

## 环境变量（GDAL/PROJ/OSG）

| 变量 | 说明 | 示例 |
|------|------|------|
| `PROJ_DATA` | PROJ 数据目录（含 proj.db） | `.\proj` |
| `PROJ_LIB` | 同上（兼容旧版） | `.\proj` |
| `OSG_PLUGIN_PATH` | OSG 插件目录（含 osgdb_osgb.dll） | `.\osgPlugins-3.6.5` |

> **程序启动时会自动搜索 exe 同目录下的 `proj/proj.db` 并设置 PROJ_DATA**，通常无需手动配置。

---

## 输出目录结构

```
<输出目录>/
  tileset.json              ← 全局入口（Cesium 加载此文件）
  Data/
    Tile_+007_+009/
      tileset.json          ← 块级 LOD 树
      Tile_+007_+009.b3dm   ← 最低 LOD（根节点）
      Tile_+007_+009_L15_0u.b3dm
      ...（多级 LOD 节点）
    Tile_+007_+010/
      ...
```

---

## 使用示例

```bat
:: 最简用法（自动从 metadata.xml 读取坐标，KTX2 纹理）
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles

:: 手动指定坐标 + 4 线程 + JPEG 纹理
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles ^
  --lon 120.266674 --lat 36.260571 --alt 0 ^
  --threads 4 --tex-format jpg --jpeg-quality 80

:: KTX2 高质量模式（UASTC）+ Draco 几何压缩
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles ^
  --tex-format ktx2 --ktx2-mode uastc --ktx2-quality 4 ^
  --draco --draco-bits 14

:: WebP 无损纹理 + 启用网格简化
osgb2tiles.exe -i D:\data\OSGB -o D:\output\3DTiles ^
  --tex-format webp --webp-lossless ^
  --simplify --simplify-ratio 0.7

:: 使用配置文件（包含七参数等完整配置）
osgb2tiles.exe -i D:\data\OSGB --config osgb2tiles_config.json
```

---

## Cesium 加载方法

```javascript
// CesiumJS 本地加载
const tileset = await Cesium.Cesium3DTileset.fromUrl(
    'http://localhost:8080/3DTiles/tileset.json'
);
viewer.scene.primitives.add(tileset);
```

或使用 `http-server`（Node.js）快速预览：
```bat
npx http-server D:\output\3DTiles --cors -p 8080
```
