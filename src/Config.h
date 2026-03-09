#pragma once

#include <string>

/**
 * @brief 七参数 Bursa-Wolf 坐标转换参数
 *
 * 用途：将 OSGB 原始局部坐标系（地方独立坐标系）转换到 WGS84（3D Tiles
 * 目标坐标系） 仅当 OSGB 数据使用地方独立坐标系，且已知与 WGS84
 * 的精确转换参数时才启用。
 *
 * 变换方向：地方 ECEF → WGS84 ECEF
 * 公式：X_wgs84 = (1 + m*1e-6) * R(rx,ry,rz) * X_local + T
 */
struct Transform7P {
  bool enabled = false; ///< 是否启用七参数转换
  double dx = 0.0;      ///< X轴平移（米）
  double dy = 0.0;      ///< Y轴平移（米）
  double dz = 0.0;      ///< Z轴平移（米）
  double rx = 0.0;      ///< X轴旋转（角秒）
  double ry = 0.0;      ///< Y轴旋转（角秒）
  double rz = 0.0;      ///< Z轴旋转（角秒）
  double scale = 0.0;   ///< 比例因子（ppm，百万分之一）
};

/**
 * @brief 所有转换配置项的聚合结构体
 * 支持从 JSON 文件或命令行参数加载
 */
struct ConvertConfig {
  // ── 输入输出路径 ──────────────────────────────────────────
  std::string inputPath;  ///< OSGB 根目录或单文件
  std::string outputPath; ///< 3D Tiles 输出目录

  // ── 地理坐标原点（WGS84） ─────────────────────────────────
  double longitude = 0.0; ///< 经度（度）
  double latitude = 0.0;  ///< 纬度（度）
  double height = 0.0;    ///< 高程（米，WGS84椭球面以上）

  // ── 七参数坐标转换（地方坐标系 → WGS84） ─────────────────
  Transform7P transform7p; ///< Bursa-Wolf 七参数，仅地方独立坐标系数据需要

  // ── 网格优化 ──────────────────────────────────────────────
  /// 根节点合并层级：-1=自动计算（推荐），0=不合并，N=强制N层
  /// 自动模式：以目标≤100个根节点为准，ceil(log4(totalBlocks/100))
  int mergeLevel = -1;
  bool simplifyMesh = false;   ///< 启用网格简化
  float simplifyRatio = 0.5f;  ///< 简化目标比例 [0.1 ~ 1.0]，越小三角形越少
  float simplifyError = 0.01f; ///< 简化最大允许误差（归一化，0~1）

  // ── 几何压缩 ──────────────────────────────────────────────
  bool compressGeometry = false; ///< Draco 几何压缩（减体积~70%，需客户端支持）
  int dracoQuantBits = 14;       ///< Draco 量化位数（位置）

  // ── 几何误差 LOD ──────────────────────────────────────────────
  double geometricErrorScale =
      0.5; ///< geometricError = bbox对角线 * scale（越大加载越激进）

  // ── 纹理处理 ──────────────────────────────────────────────
  // ktx2Mode/ktx2Quality/ktx2Mipmaps
  std::string ktx2Mode = "etc1s"; ///< "etc1s"(OSGB) | "uastc"(BIM)
  int ktx2Quality = 2;            ///< 1-5
  bool ktx2Mipmaps = true;        ///< generate mip chain (strongly recommended)

  bool compressTexture = true;        ///< enable texture compression
  std::string textureFormat = "ktx2"; ///< ktx2 / jpg / png / webp
  int maxTextureSize = 2048;          ///< 最大纹理尺寸（像素）
  int jpegQuality = 85;               ///< JPEG 质量 [1-100]
  int webpQuality = 80;               ///< WebP 质量 [1-100]（lossy）
  bool webpLossless = false;          ///< WebP 无损模式

  // ── 性能 ──────────────────────────────────────────────────
  int threads = 4;           ///< 并行线程数（0 = 自动匹配 CPU 核心数）
  size_t maxMemoryMB = 4096; ///< 内存使用上限（MB，超出时暂停新任务）

  // ── 输出格式 ──────────────────────────────────────────────
  std::string tileFormat = "b3dm";    ///< 瓦片格式: b3dm / glb
  std::string refineMode = "REPLACE"; ///< LOD 切换模式: REPLACE / ADD
  bool writeGzip = false;             ///< 是否对输出文件 gzip 压缩

  // ── 调试 ──────────────────────────────────────────────────
  bool verbose = false; ///< 详细日志
  bool dryRun = false;  ///< 演习模式（不写文件）
};
