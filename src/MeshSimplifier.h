#pragma once

#include "OsgbReader.h"
#include "Config.h"

/**
 * @brief 基于 meshoptimizer 的网格简化器
 */
class MeshSimplifier {
public:
    explicit MeshSimplifier(const ConvertConfig& cfg);

    /**
     * @brief 简化 node 中的三角网
     * 修改 node.vertices / node.normals / node.uvs / node.indices
     */
    void simplify(TileNode& node);

private:
    ConvertConfig cfg_;

    /// 顶点缓存优化（提升 GPU 渲染效率）
    void optimizeVertexCache(TileNode& node);
};
