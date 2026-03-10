#pragma once

#include "Config.h"
#include <nlohmann/json.hpp>
#include <osg/BoundingBox>
#include <string>
#include <vector>

/**
 * @brief 根节点合并信息（来自 main.cpp 的 BlockResult）
 */
struct MergeBlockInfo {
  std::string blockName;           ///< Block 名称，如 "Tile_+007_+009"
  std::string blockTilesetRelPath; ///< 相对输出目录的 tileset 路径
  osg::BoundingBox bbox;           ///< Block 级别包围盒（局部 ENU 坐标）
  double geometricError = 6.0;     ///< Block 根节点的 geometricError
  std::string rootTilePath;        ///< Block 根瓦片（b3dm/glb）的绝对路径
};

/**
 * @brief 合并树节点（内部数据结构）
 */
struct MergeTreeNode {
  std::string name;            ///< 节点名，如 "Top", "TopA", "TopAB"
  osg::BoundingBox bbox;       ///< 合并包围盒
  double geometricError = 0.0; ///< 此节点的 geometricError

  std::vector<int> blockIndices; ///< 此节点关联的 Block 索引
  std::vector<MergeTreeNode>
      children; ///< 子节点（内部节点 2~4 个，叶节点 0 个）

  bool isLeaf() const { return children.empty(); }
};

/**
 * @brief 顶层根节点合并器
 *
 * 后处理阶段：在所有 Block 转换完成后，读取各 Block 根节点的 b3dm/glb，
 * 跨 Block 合并几何体 + meshoptimizer 激进简化 + 纹理 atlas 拼合，
 * 生成 top/ 目录。
 *
 * 功能完全独立，不影响现有转换流程。
 */
class TopLevelMerger {
public:
  /**
   * @brief 执行根节点合并
   * @param blocks     所有已转换的 Block 信息
   * @param cfg        当前转换配置
   * @param outputDir  输出根目录（与 tileset.json 同级）
   * @return true if success
   */
  bool merge(const std::vector<MergeBlockInfo> &blocks,
             const ConvertConfig &cfg, const std::string &outputDir);

private:
  // ── Step 1: 构建合并树 ────────────────────────────────────────
  MergeTreeNode buildMergeTree(const std::vector<MergeBlockInfo> &blocks,
                               int maxLeafBlocks = 3, int maxDepth = 6);

  void splitNode(MergeTreeNode &node, const std::vector<MergeBlockInfo> &blocks,
                 const std::string &prefix, int depth, int maxLeafBlocks,
                 int maxDepth);

  // ── Step 2: 几何合并 + 简化 ───────────────────────────────────

  /// 合并后的几何数据
  struct MergedMesh {
    std::vector<float> vertices;   ///< xyz 交织
    std::vector<float> normals;    ///< xyz 交织
    std::vector<float> uvs;        ///< st 交织
    std::vector<uint32_t> indices; ///< 三角形索引
    std::vector<uint8_t> atlasRGB; ///< atlas 纹理 RGB 像素
    int atlasWidth = 0;
    int atlasHeight = 0;
  };

  /// 从 b3dm/glb 中提取的单个 Block 几何数据
  struct ExtractedBlock {
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
    std::vector<uint8_t> texRGB; ///< 纹理像素 RGB
    int texWidth = 0;
    int texHeight = 0;
  };

  /// 读取 b3dm/glb 文件并提取几何 + 纹理
  bool extractFromTile(const std::string &tilePath, ExtractedBlock &out);

  /// 合并多个 Block 几何为一个 mesh + atlas
  MergedMesh mergeBlocks(const std::vector<MergeBlockInfo> &blocks,
                         const std::vector<int> &blockIndices,
                         float simplifyRatio = 0.1f);

  /// 创建纹理 atlas (简单 strip 排列)
  void buildAtlas(const std::vector<ExtractedBlock> &extracted,
                  std::vector<uint8_t> &atlasRGB, int &atlasW, int &atlasH,
                  std::vector<std::array<float, 4>> &uvRegions);

  /// meshoptimizer 激进简化
  void simplifyMesh(MergedMesh &mesh, float targetRatio);

  // ── Step 3: 写出 ─────────────────────────────────────────────
  /// 将 MergedMesh 写为 b3dm/glb 文件
  bool writeMergedTile(const MergedMesh &mesh, const std::string &outPath,
                       const ConvertConfig &cfg);

  /// 递归生成 top/tileset.json
  nlohmann::json buildTopTilesetNode(const MergeTreeNode &node,
                                     const std::vector<MergeBlockInfo> &blocks);

  /// 更新根 tileset.json（备份原始为 _tileset.json）
  bool updateRootTileset(const std::string &outputDir,
                         const osg::BoundingBox &globalBbox);
};
