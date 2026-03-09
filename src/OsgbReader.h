#pragma once

#include <array>
#include <functional>
#include <osg/BoundingBox>
#include <osg/Node>
#include <string>
#include <vector>

/**
 * @brief 单个 Geode（几何子对象）的提取结果
 * 每个 Geode 有独立几何数据和纹理，对应 glTF 的一个 Primitive + Material
 */
struct SubMesh {
  std::vector<float> vertices;   ///< xyz 交织
  std::vector<float> normals;    ///< xyz 交织
  std::vector<float> uvs;        ///< st 交织（V 轴已翻转到 glTF 惯例）
  std::vector<uint32_t> indices; ///< 三角形索引

  // 纹理
  std::string texturePath;          ///< 磁盘纹理路径（可空）
  std::vector<uint8_t> textureData; ///< 解码后的 RGB 像素（rows top-to-bottom）
  int texWidth = 0;
  int texHeight = 0;
  std::vector<uint8_t>
      encodedData; ///< 编码后的 JPEG/PNG/WebP（由 TextureProcessor 填充）
};

/**
 * @brief 单个瓦片节点的提取结果
 */
struct TileNode {
  std::string nodeId;                ///< 节点唯一ID（路径哈希）
  std::string osgbPath;              ///< 对应 .osgb 文件的绝对路径
  int level = 0;                     ///< LOD 层级（0=根节点）
  osg::BoundingBox bbox;             ///< 本地坐标系包围盒
  double geometricError = 0.0;       ///< 像素误差（米）
  std::vector<std::string> children; ///< 子节点 nodeId 列表

  // 多 SubMesh 支持（每个 Geode 对应一个 SubMesh）
  std::vector<SubMesh> subMeshes; ///< 每个 Geode/Drawable 对应一个子网格

  // 以下字段保留用于向后兼容（最终写入时以 subMeshes 为准）
  std::array<double, 3> rtcCenter = {0.0, 0.0, 0.0}; ///< 保留兼容字段

  // 纹理（兼容旧路径，当 subMeshes 仅含一个）
  std::string texturePath;
  std::vector<uint8_t> textureData;
  int texWidth = 0, texHeight = 0;

  // 几何数据合并（兼容旧接口）
  std::vector<float> vertices;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<uint32_t> indices;
};

/**
 * @brief OSGB 场景图读取器
 * 递归遍历 PagedLOD 树，提取每个叶子节点的几何数据
 */
class OsgbReader {
public:
  explicit OsgbReader(bool verbose = false);

  /**
   * @brief 扫描 OSGB 根目录，构建瓦片树
   * @param rootDir  OSGB 数据根目录（含 metadata.xml 或直接含 .osgb 文件）
   * @return 根节点列表（可能有多个，若未执行根节点合并）
   */
  std::vector<TileNode> scan(const std::string &rootDir);

  /**
   * @brief 读取单个 .osgb 文件，从中提取几何数据填充到 node
   * @param node  已有 osgbPath 的 TileNode（in/out）
   * @return true if success
   */
  bool loadGeometry(TileNode &node);

  /**
   * @brief 【Stage 1 专用】只做 osgDB::readNodeFile，不提取几何。
   *        受 OSG 全局锁保护，调用者必须保证单线程调用。
   * @param osgbPath    .osgb 文件的绝对路径
   * @return 加载成功的 osg::Node（失败返回 nullptr）
   */
  osg::ref_ptr<osg::Node> readFile(const std::string &osgbPath);

  /**
   * @brief 【Stage 2 专用】从已加载的 osgNode 提取几何+纹理像素。
   *        不调用 osgDB，无全局锁，可在 Worker 线程并行调用。
   * @param osgNode     由 readFile() 返回的 osg::Node
   * @param osgbPath    原始文件路径（用于纹理路径解析）
   * @param node        in/out，已有 nodeId/level 等元数据
   * @return true if success
   */
  bool extractFromNode(osg::ref_ptr<osg::Node> osgNode,
                       const std::string &osgbPath, TileNode &node);

  /**
   * @brief 遍历回调（调试用）
   */
  using VisitCallback = std::function<void(const TileNode &)>;
  void setVisitCallback(VisitCallback cb) { visitCb_ = cb; }

private:
  bool verbose_;
  VisitCallback visitCb_;

  /// 递归遍历 OSG 节点树，收集 PagedLOD 信息
  void traverseNode(osg::Node *node, TileNode &out, int level,
                    const std::string &baseDir);

  /// 从 osg::Geode 提取顶点/法线/UV/索引，并重建缺失法线
  void extractGeometry(osg::Node *geodeNode, TileNode &out);

  /// 从 osg::StateSet 提取纹理文件路径
  std::string extractTexturePath(osg::Node *node);

  /// 重建顶点法线（当原始数据无法线时）
  void rebuildNormals(TileNode &node);
};
