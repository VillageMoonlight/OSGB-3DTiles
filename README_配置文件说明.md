# osgb2tiles 配置文件说明（JSON 格式）

GUI 保存的配置文件 `osgb2tiles_config.json` 可直接通过 `--config` 参数传给命令行程序。

---

## 完整示例

```json
{
  "inputPath":       "D:\\data\\OSGB",
  "outputPath":      "D:\\output\\3DTiles",

  "longitude":       120.266674,
  "latitude":        36.260571,
  "height":          0.0,

  "tileFormat":      "b3dm",
  "threads":         4,

  "simplifyMesh":    false,
  "simplifyRatio":   0.5,

  "textureFormat":   "ktx2",
  "compressTexture": true,
  "maxTextureSize":  2048,

  "ktx2Mode":        "etc1s",
  "ktx2Quality":     2,
  "ktx2Mipmaps":     true,

  "jpegQuality":     85,

  "webpQuality":     80,
  "webpLossless":    false,

  "compressGeometry": false,
  "dracoQuantBits":   14,

  "geometricErrorScale": 0.5,

  "verbose":         false,

  "transform7p": {
    "enabled": false,
    "dx":    0.0,
    "dy":    0.0,
    "dz":    0.0,
    "rx":    0.0,
    "ry":    0.0,
    "rz":    0.0,
    "scale": 0.0
  }
}
```

---

## 字段说明

### 路径

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `inputPath` | 字符串 | ✅ | OSGB 数据输入目录（绝对路径） |
| `outputPath` | 字符串 | ✅ | 3D Tiles 输出目录（不存在则自动创建） |

### 地理坐标原点（WGS84）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `longitude` | 浮点 | 自动检测 | 原点经度，十进制度，东经为正 |
| `latitude` | 浮点 | 自动检测 | 原点纬度，十进制度，北纬为正 |
| `height` | 浮点 | `0.0` | 原点椭球高（米） |

> 若 JSON 中未填写坐标或填 0，程序自动从 `metadata.xml` 读取。

### 输出格式

| 字段 | 类型 | 可选值 | 默认 | 说明 |
|------|------|--------|------|------|
| `tileFormat` | 字符串 | `"b3dm"` \| `"glb"` | `"b3dm"` | b3dm 为 Cesium 3D Tiles 标准格式；glb 为独立 GLTF 文件 |
| `threads` | 整数 | 1~64 | `4` | 并行线程数，0 = 自动使用所有核心 |

### 网格简化

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `simplifyMesh` | 布尔 | `false` | 是否启用三角网简化 |
| `simplifyRatio` | 浮点 | `0.5` | 简化比例 `[0.1~1.0]`，0.5 = 保留 50% 的面 |

### 纹理压缩

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `textureFormat` | 字符串 | `"ktx2"` | 目标纹理格式：`ktx2` / `jpg` / `png` / `webp` |
| `compressTexture` | 布尔 | `false` | 是否启用纹理格式转换 |
| `maxTextureSize` | 整数 | `2048` | 纹理最大宽/高像素，超过则等比例降采样 |

**KTX2 选项**（`textureFormat = "ktx2"` 时有效）：

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `ktx2Mode` | 字符串 | `"etc1s"` | 压缩模式：`etc1s`（小体积，适合倾斜摄影）或 `uastc`（高质量，适合精细模型） |
| `ktx2Quality` | 整数 | `2` | 压缩质量 `[1-5]`，越大越慢但质量越好 |
| `ktx2Mipmaps` | 布尔 | `true` | 是否生成 Mipmaps（强烈建议开启，减少远处走样） |

**JPEG 选项**（`textureFormat = "jpg"` 时有效）：

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `jpegQuality` | 整数 | `85` | JPEG 压缩质量 `[1-100]`，85 = 高质量平衡 |

**WebP 选项**（`textureFormat = "webp"` 时有效）：

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `webpQuality` | 整数 | `80` | WebP 有损质量 `[1-100]` |
| `webpLossless` | 布尔 | `false` | 启用 WebP 无损模式（忽略 webpQuality） |

### 几何压缩（Draco）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `compressGeometry` | 布尔 | `false` | 是否启用 Draco 几何压缩，体积可减约 70% |
| `dracoQuantBits` | 整数 | `14` | Draco 位置量化位数 `[8-16]`，越小压缩率越高但精度损失越大 |

### LOD 误差控制

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `geometricErrorScale` | 浮点 | `0.5` | geometricError 比例系数 `[0.1~2.0]`。越大 LOD 切换越激进，加载越快但细节提前显示 |

### 其他

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `verbose` | 布尔 | `false` | 是否打印详细调试日志 |

### 7 参数坐标转换（可选）

`transform7p` 为可选字段，用于将地方独立坐标系精准对齐到 WGS84（Bursa-Wolf 模型）：

$$X_{WGS84} = (1 + m) \cdot R \cdot X_{local} + T$$

| 字段 | 类型 | 单位 | 说明 |
|------|------|------|------|
| `enabled` | 布尔 | — | 是否启用七参数转换 |
| `dx` | 浮点 | 米 | X 轴平移量 |
| `dy` | 浮点 | 米 | Y 轴平移量 |
| `dz` | 浮点 | 米 | Z 轴平移量 |
| `rx` | 浮点 | 角秒 | X 轴旋转角 |
| `ry` | 浮点 | 角秒 | Y 轴旋转角 |
| `rz` | 浮点 | 角秒 | Z 轴旋转角 |
| `scale` | 浮点 | ppm | 比例因子（0 = 无缩放） |

> 若不需要七参转换，可整体省略 `transform7p` 字段，或将 `enabled` 设为 `false`。

---

## 配置文件优先级

命令行参数 **优先于** 配置文件中的同名字段。  
例如：`--lon 120.5 --config cfg.json`，则经度使用 `120.5` 而非 cfg.json 中的值。

---

## 最小配置文件（仅路径）

```json
{
  "inputPath":  "D:\\data\\OSGB",
  "outputPath": "D:\\output\\3DTiles"
}
```

程序将从 `metadata.xml` 自动读取坐标，其余选项使用默认值。

---

## 纹理格式选择建议

| 场景 | 推荐格式 | 理由 |
|------|----------|------|
| 倾斜摄影（照片纹理） | `ktx2 + etc1s` | 体积最小，GPU 直读 |
| 精细 BIM 模型 | `ktx2 + uastc` | 质量更高，保留更多细节 |
| 需要最大兼容性 | `jpg` | 所有浏览器/引擎均支持 |
| 含透明度的纹理 | `png` 或 `webp` | 支持 alpha 通道 |
| 高质量无损 | `webp --webp-lossless` | 比 PNG 体积小约 20-30% |
