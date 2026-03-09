#include "TilesetBuilder.h"
#include "Logger.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cmath>

#include <cmath>
#include <cstddef>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    constexpr double PI = 3.14159265358979323846;
    inline double toRad(double deg) { return deg * PI / 180.0; }
}

TilesetBuilder::TilesetBuilder(const ConvertConfig& cfg,
                               const MathUtils::Mat4d& transform)
    : cfg_(cfg), transform_(transform) {}

bool TilesetBuilder::build(const std::vector<TileNode>& rootNodes,
                            const std::string& outputDir)
{
    json tileset;

    // ── 资产元数据 ────────────────────────────────────────────────
    tileset["asset"] = {
        {"version", "1.0"},
        {"tilesetVersion", "1.0"},
        {"gltfUpAxis", "Y"}
    };

    // ── 几何误差（根节点） ─────────────────────────────────────────
    tileset["geometricError"] = 1024.0;

    // ── ENU→ECEF 变换矩阵（列主序 16 个 double） ──────────────────
    std::vector<double> xfm(transform_.begin(), transform_.end());

    // ── 根节点  ───────────────────────────────────────────────────
    if (rootNodes.empty()) {
        LOG_WARN("No root tiles to build tileset.");
        return false;
    }

    json rootContent;
    if (rootNodes.size() == 1) {
        rootContent = buildTileJson(rootNodes[0], outputDir, 0);
        rootContent["transform"] = xfm;
    } else {
        // 多根节点：合并为一个虚拟根
        json children = json::array();
        double maxError = 0.0;
        json mergedBbox;

        for (const auto& rn : rootNodes) {
            json child = buildTileJson(rn, outputDir, 0);
            maxError = std::max(maxError, child["geometricError"].get<double>());
            children.push_back(std::move(child));
        }

        // 虚根包围盒取第一个子节点的（后续可改为合并）
        rootContent = {
            {"boundingVolume", buildTileJson(rootNodes[0], outputDir, 0)["boundingVolume"]},
            {"geometricError", maxError * 2.0},
            {"refine", cfg_.refineMode},
            {"transform", xfm},
            {"children", std::move(children)}
        };
    }

    tileset["root"] = rootContent;

    // ── 写文件 ────────────────────────────────────────────────────
    std::string outPath = outputDir + "/tileset.json";
    std::ofstream ofs(outPath);
    if (!ofs) {
        LOG_ERROR("Cannot write tileset.json: " + outPath);
        return false;
    }
    ofs << tileset.dump(2);
    LOG_INFO("Wrote tileset.json -> " + outPath);
    return true;
}

json TilesetBuilder::buildTileJson(const TileNode& node,
                                    const std::string& outputDir,
                                    int depth)
{
    json tile;

    // 包围盒
    tile["boundingVolume"] = calcBoundingBox(node);

    // 几何误差
    tile["geometricError"] = calcGeometricError(node, depth);
    tile["refine"] = cfg_.refineMode;

    // 内容 URI（文件名与 nodeId 对应）
    std::string ext = (cfg_.tileFormat == "b3dm") ? ".b3dm" : ".glb";
    std::string uri = "tiles/" + node.nodeId + ext;
    tile["content"] = {{"uri", uri}};

    // 子节点（递归加载，Cesium 按需请求）
    if (!node.children.empty()) {
        json children = json::array();
        for (const auto& childPath : node.children) {
            // 子节点仅记录 URI，实际内容由 Cesium 懒加载
            TileNode childNode;
            childNode.nodeId   = fs::path(childPath).stem().string();
            childNode.osgbPath = childPath;
            childNode.level    = depth + 1;
            // 包围盒由实际加载时确定，这里用父节点的1/4大小估算
            childNode.bbox = node.bbox;

            children.push_back(buildTileJson(childNode, outputDir, depth + 1));
        }
        tile["children"] = std::move(children);
    }

    return tile;
}

double TilesetBuilder::calcGeometricError(const TileNode& node, int depth) {
    if (node.geometricError > 0.0) return node.geometricError;

    // OSG BoundingBox 有效时从对角线推算
    // (默认值 _min=MAX_FLT/_max=-MAX_FLT 时 valid() 返回 false)
    if (node.bbox.valid()) {
        float dx = node.bbox._max.x() - node.bbox._min.x();
        float dy = node.bbox._max.y() - node.bbox._min.y();
        float dz = node.bbox._max.z() - node.bbox._min.z();
        double diagLen = std::sqrt((double)(dx*dx + dy*dy + dz*dz));
        if (std::isfinite(diagLen) && diagLen > 0.0)
            return diagLen * 0.5 * std::pow(0.5, depth);
    }

    // bbox 未知时：按层级给默认值（单位：米）
    return 512.0 * std::pow(0.5, depth);
}

json TilesetBuilder::calcBoundingBox(const TileNode& node) {
    // bbox 有效且尺寸合理时，用 box 格式（局部坐标系，配合 transform 矩阵）
    if (node.bbox.valid()) {
        float dx = node.bbox._max.x() - node.bbox._min.x();
        float dy = node.bbox._max.y() - node.bbox._min.y();
        float dz = node.bbox._max.z() - node.bbox._min.z();
        float cx = (node.bbox._min.x() + node.bbox._max.x()) * 0.5f;
        float cy = (node.bbox._min.y() + node.bbox._max.y()) * 0.5f;
        float cz = (node.bbox._min.z() + node.bbox._max.z()) * 0.5f;
        float hx = dx * 0.5f, hy = dy * 0.5f, hz = dz * 0.5f;

        if (std::isfinite(cx) && std::isfinite(hx) && hx > 0.0f) {
            return {{"box", {
                cx, cy, cz,
                hx, 0,  0,
                0,  hy, 0,
                0,  0,  hz
            }}};
        }
    }

    // bbox 未填充（或无效），使用 region 格式基于已知地理中心估算
    // region: [west, south, east, north, minHeight, maxHeight]（弧度/米）
    double lonRad = toRad(cfg_.longitude);
    double latRad = toRad(cfg_.latitude);

    // 1 度经度 ≈ 111km，给整块数据约 ±5km/±3km 的边距
    double dLon = toRad(0.05); // ±5km 横向
    double dLat = toRad(0.03); // ±3km 纵向
    double hMin = cfg_.height - 50.0;
    double hMax = cfg_.height + 200.0;

    return {{"region", {
        lonRad - dLon,  // west
        latRad - dLat,  // south
        lonRad + dLon,  // east
        latRad + dLat,  // north
        hMin,           // minHeight
        hMax            // maxHeight
    }}};
}
