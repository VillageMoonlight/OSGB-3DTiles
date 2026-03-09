#include "MeshSimplifier.h"
#include "Logger.h"

#include <meshoptimizer.h>
#include <cstring>
#include <cassert>

MeshSimplifier::MeshSimplifier(const ConvertConfig& cfg) : cfg_(cfg) {}

void MeshSimplifier::simplify(TileNode& node) {
    if (!cfg_.simplifyMesh) return;
    if (node.indices.size() < 9) return; // 少于 3 个三角形不简化

    const size_t vtxCount = node.vertices.size() / 3;
    const size_t idxCount = node.indices.size();

    // ── 目标三角形数 ────────────────────────────────────────────
    size_t targetIdxCount = static_cast<size_t>(
        idxCount * std::max(0.05f, std::min(1.0f, cfg_.simplifyRatio)) / 3) * 3;

    if (targetIdxCount >= idxCount) {
        LOG_DEBUG("Skip simplify (ratio >= 1.0): " + node.nodeId);
        optimizeVertexCache(node);
        return;
    }

    LOG_DEBUG("Simplify " + node.nodeId +
              ": " + std::to_string(idxCount/3) + " -> ~" +
              std::to_string(targetIdxCount/3) + " tris");

    std::vector<uint32_t> simplifiedIdx(idxCount);

    // meshoptimizer 需要顶点stride（字节）
    const size_t vtxStride = sizeof(float) * 3;

    size_t resultIdxCount = meshopt_simplify(
        simplifiedIdx.data(),
        node.indices.data(),
        idxCount,
        node.vertices.data(),
        vtxCount,
        vtxStride,
        targetIdxCount,
        cfg_.simplifyError
    );

    simplifiedIdx.resize(resultIdxCount);

    LOG_DEBUG("  -> " + std::to_string(resultIdxCount/3) + " tris after simplify");

    // ── 重建顶点数组（只保留引用到的顶点） ──────────────────────
    // 用 meshopt_generateVertexRemap 去除冗余顶点
    std::vector<uint32_t> remap(vtxCount);
    size_t newVtxCount = meshopt_generateVertexRemap(
        remap.data(),
        simplifiedIdx.data(),
        resultIdxCount,
        node.vertices.data(),
        vtxCount,
        vtxStride
    );

    // 重映射顶点
    std::vector<float> newVerts(newVtxCount * 3);
    meshopt_remapVertexBuffer(newVerts.data(),
                              node.vertices.data(),
                              vtxCount, vtxStride,
                              remap.data());

    // 重映射法线
    std::vector<float> newNorms;
    if (!node.normals.empty()) {
        newNorms.resize(newVtxCount * 3);
        meshopt_remapVertexBuffer(newNorms.data(),
                                  node.normals.data(),
                                  vtxCount, vtxStride,
                                  remap.data());
    }

    // 重映射 UV
    std::vector<float> newUVs;
    if (!node.uvs.empty()) {
        newUVs.resize(newVtxCount * 2);
        meshopt_remapVertexBuffer(newUVs.data(),
                                  node.uvs.data(),
                                  vtxCount, sizeof(float)*2,
                                  remap.data());
    }

    // 重映射索引
    std::vector<uint32_t> newIdx(resultIdxCount);
    meshopt_remapIndexBuffer(newIdx.data(),
                             simplifiedIdx.data(),
                             resultIdxCount,
                             remap.data());

    // 应用结果
    node.vertices = std::move(newVerts);
    node.normals  = std::move(newNorms);
    node.uvs      = std::move(newUVs);
    node.indices  = std::move(newIdx);

    optimizeVertexCache(node);
}

void MeshSimplifier::optimizeVertexCache(TileNode& node) {
    meshopt_optimizeVertexCache(
        node.indices.data(),
        node.indices.data(),
        node.indices.size(),
        node.vertices.size() / 3
    );
}
