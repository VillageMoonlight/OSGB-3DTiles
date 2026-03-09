#pragma once

#include "OsgbReader.h"
#include "Config.h"
#include "MathUtils.h"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>


/**
 * @brief 递归生成 3D Tiles tileset.json 层次结构
 */
class TilesetBuilder {
public:
    TilesetBuilder(const ConvertConfig& cfg,
                   const MathUtils::Mat4d& transform);

    /**
     * @brief 生成完整的 tileset.json 并写入磁盘
     * @param rootNodes  顶层瓦片列表
     * @param outputDir  输出目录（tileset.json 写入此目录）
     */
    bool build(const std::vector<TileNode>& rootNodes,
               const std::string& outputDir);

private:
    ConvertConfig    cfg_;
    MathUtils::Mat4d transform_; ///< ENU→ECEF 变换矩阵（写入 root.transform）

    /// 递归构建单个瓦片的 JSON 节点
    nlohmann::json buildTileJson(const TileNode& node,
                                  const std::string& outputDir,
                                  int depth);

    /// 计算 geometricError（基于包围盒尺寸）
    double calcGeometricError(const TileNode& node, int depth);

    /// 计算 boundingVolume.box（OBB 格式，12个 double）
    nlohmann::json calcBoundingBox(const TileNode& node);
};
