#pragma once

#include "OsgbReader.h"
#include "MathUtils.h"
#include "Config.h"

/**
 * @brief 将 TileNode 局部坐标转换为 ECEF，并应用 RTC 优化
 *
 * 变换流程：
 *   OSGB 局部坐标（ENU，以 SRS Origin 为原点）
 *     → [可选] 七参数 Bursa-Wolf 旋转/平移（地方坐标系 → WGS84）
 *     → ECEF（全球笛卡尔坐标系）
 *     → 相对瓦片中心（RTC，保留双精度）
 */
class GeometryConverter {
public:
    explicit GeometryConverter(const ConvertConfig& cfg);

    /**
     * @brief 对 node 中的所有顶点执行坐标变换
     *   局部坐标（OSGB ENU 原点系）→ [可选7参] → ECEF → RTC
     * 变换后 node.vertices 存储 RTC 坐标，node.rtcCenter 存储 ECEF 中心点
     */
    void transform(TileNode& node);

    /**
     * @brief 返回 ENU→ECEF 的 4x4 变换矩阵（列主序，用于 tileset.json transform）
     */
    const MathUtils::Mat4d& getTransformMatrix() const { return enu2ecef_; }

private:
    ConvertConfig    cfg_;
    MathUtils::Mat4d enu2ecef_;   ///< 局部ENU → 全局ECEF 矩阵
    MathUtils::Vec3d ecefOrigin_; ///< 原点的 ECEF 坐标

    /**
     * @brief 对单个 ECEF 坐标点施加 Bursa-Wolf 七参变换
     * 变换方向：地方 ECEF 大地坐标系 → WGS84 ECEF
     * @param pt  输入/输出点（ECEF，单位米）
     */
    void applyBursaWolf7P(MathUtils::Vec3d& pt) const;
};
