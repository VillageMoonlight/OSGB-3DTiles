#include "GeometryConverter.h"
#include "Logger.h"
#include <cmath>

GeometryConverter::GeometryConverter(const ConvertConfig& cfg)
    : cfg_(cfg)
{
    enu2ecef_   = MathUtils::enuToEcefMatrix(cfg.longitude, cfg.latitude);
    ecefOrigin_ = MathUtils::llaToEcef(cfg.longitude, cfg.latitude, cfg.height);
}

// ─── Bursa-Wolf 七参变换 ──────────────────────────────────────────────────────
// 变换方向：地方坐标系 ECEF → WGS84 ECEF
// 公式（Position Vector 约定，符合 EPSG:9606）：
//   X_wgs84 = (1 + m) * R(rx, ry, rz) * X_local + T
// 其中 m = scale * 1e-6（ppm→无量纲），rx/ry/rz 单位弧度
void GeometryConverter::applyBursaWolf7P(MathUtils::Vec3d& pt) const {
    const Transform7P& p = cfg_.transform7p;

    // 比例因子（ppm → 无量纲）
    double m = 1.0 + p.scale * 1e-6;

    // 旋转角（角秒 → 弧度）
    constexpr double ARCSEC_TO_RAD = M_PI / (180.0 * 3600.0);
    double rx = p.rx * ARCSEC_TO_RAD;
    double ry = p.ry * ARCSEC_TO_RAD;
    double rz = p.rz * ARCSEC_TO_RAD;

    // 小角度近似旋转矩阵（Position Vector 符号约定）：
    //   R = [ 1    -rz   ry  ]
    //       [ rz    1   -rx  ]
    //       [-ry    rx    1  ]
    double x0 = pt[0], y0 = pt[1], z0 = pt[2];

    pt[0] = m * (      x0 - rz * y0 + ry * z0) + p.dx;
    pt[1] = m * ( rz * x0 +      y0 - rx * z0) + p.dy;
    pt[2] = m * (-ry * x0 + rx * y0 +      z0) + p.dz;
}

// ─── 坐标变换主方法（Pattern 1：顶点保持本地 ENU，由 tileset.json transform 处理 ECEF）──
void GeometryConverter::transform(TileNode& node) {
    if (node.vertices.empty()) return;

    const size_t vtxCount = node.vertices.size() / 3;

    // Pattern 1: 顶点保持 OSGB 本地 ENU 坐标（米），不做 ECEF 变换
    // tileset.json 根节点的 transform 矩阵（ENU→ECEF）由 main.cpp 写入
    // Cesium 在渲染时自动将顶点从 ENU 转到 ECEF 世界坐标

    // rtcCenter 设为零（Pattern 1 不使用 CESIUM_RTC）
    node.rtcCenter = {0.0, 0.0, 0.0};

    // 若不启用七参数，直接返回（顶点无需任何变换）
    if (!cfg_.transform7p.enabled) {
        LOG_DEBUG("Vertices kept in local ENU space (" + std::to_string(vtxCount) + " verts).");
        return;
    }

    // 启用七参数时：局部 ENU → 局部 ECEF → Bursa-Wolf → WGS84 ECEF
    // 然后将 WGS84 ECEF 反投影回 ENU（以保持与 tileset.json transform 兼容）
    // 简化处理：仅对每个顶点做 7P 旋转平移，不做完整 ECEF 往返
    const Transform7P& p = cfg_.transform7p;
    constexpr double ARCSEC_TO_RAD = M_PI / (180.0 * 3600.0);
    double rx = p.rx * ARCSEC_TO_RAD;
    double ry = p.ry * ARCSEC_TO_RAD;
    double rz = p.rz * ARCSEC_TO_RAD;
    double m  = 1.0 + p.scale * 1e-6;

    for (size_t i = 0; i < vtxCount; ++i) {
        float* vp = node.vertices.data() + i * 3;
        // 先转换到 ECEF
        MathUtils::Vec3d ecef = MathUtils::transformPoint(enu2ecef_, {vp[0], vp[1], vp[2]});
        // 施加 Bursa-Wolf
        applyBursaWolf7P(ecef);
        // 写回（ECEF 相对于原点，大数值——仅在七参数模式下允许，需配合对应 tileset transform）
        vp[0] = static_cast<float>(ecef[0]);
        vp[1] = static_cast<float>(ecef[1]);
        vp[2] = static_cast<float>(ecef[2]);
    }

    LOG_DEBUG("Applied 7P Bursa-Wolf + ECEF for " + std::to_string(vtxCount) + " vertices.");
}
