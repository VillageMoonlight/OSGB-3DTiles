#pragma once

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <array>
#include <string>
#include <vector>
#include <cmath>


namespace MathUtils {

// ────────────────────────────────────────────────────────────────────────────
// WGS84 椭球体参数
// ────────────────────────────────────────────────────────────────────────────
constexpr double PI       = 3.14159265358979323846;
constexpr double WGS84_A = 6378137.0;           ///< 长半轴（米）
constexpr double WGS84_B = 6356752.3142;        ///< 短半轴（米）
constexpr double WGS84_E2 = 0.00669437999014;   ///< 第一偏心率平方
constexpr double DEG_TO_RAD = PI / 180.0;
constexpr double RAD_TO_DEG = 180.0 / PI;


using Vec3d = std::array<double, 3>;
using Vec3f = std::array<float, 3>;
using Mat4d = std::array<double, 16>;

/**
 * @brief WGS84 经纬高 → ECEF 笛卡尔坐标
 * @param lon 经度（度）
 * @param lat 纬度（度）
 * @param alt 高程（米，WGS84椭球面以上）
 */
inline Vec3d llaToEcef(double lon, double lat, double alt) {
    const double phi    = lat * DEG_TO_RAD;
    const double lambda = lon * DEG_TO_RAD;
    const double sinPhi = std::sin(phi);
    const double cosPhi = std::cos(phi);

    // 卯酉圈主曲率半径
    const double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    return {
        (N + alt) * cosPhi * std::cos(lambda),
        (N + alt) * cosPhi * std::sin(lambda),
        (N * (1.0 - WGS84_E2) + alt) * sinPhi
    };
}

/**
 * @brief 生成从局部 ENU（东北天）坐标系到 ECEF 的旋转矩阵（列主序 4x4）
 * @param lon 原点经度（度）
 * @param lat 原点纬度（度）
 */
inline Mat4d enuToEcefMatrix(double lon, double lat) {
    const double phi    = lat * DEG_TO_RAD;
    const double lambda = lon * DEG_TO_RAD;

    const double sinLon = std::sin(lambda), cosLon = std::cos(lambda);
    const double sinLat = std::sin(phi),    cosLat = std::cos(phi);

    // ENU 轴在 ECEF 中的方向
    // East  = (-sinLon,          cosLon,         0       )
    // North = (-cosLon*sinLat,  -sinLon*sinLat,  cosLat  )
    // Up    = ( cosLon*cosLat,   sinLon*cosLat,  sinLat  )
    Mat4d m = {};
    // 列 0: East
    m[0] = -sinLon;        m[1] = cosLon;          m[2]  = 0.0;     m[3]  = 0.0;
    // 列 1: North
    m[4] = -cosLon*sinLat; m[5] = -sinLon*sinLat;  m[6]  = cosLat;  m[7]  = 0.0;
    // 列 2: Up
    m[8] =  cosLon*cosLat; m[9] =  sinLon*cosLat;  m[10] = sinLat;  m[11] = 0.0;
    // 列 3: 平移（原点 ECEF）
    Vec3d org = llaToEcef(lon, lat, 0.0);
    m[12] = org[0]; m[13] = org[1]; m[14] = org[2]; m[15] = 1.0;

    return m;
}

/**
 * @brief 矩阵 × 向量（仅旋转部分，不含平移）
 */
inline Vec3d transformDir(const Mat4d& m, const Vec3d& v) {
    return {
        m[0]*v[0] + m[4]*v[1] + m[8] *v[2],
        m[1]*v[0] + m[5]*v[1] + m[9] *v[2],
        m[2]*v[0] + m[6]*v[1] + m[10]*v[2]
    };
}

/**
 * @brief 矩阵 × 点（含平移）
 */
inline Vec3d transformPoint(const Mat4d& m, const Vec3d& v) {
    return {
        m[0]*v[0] + m[4]*v[1] + m[8] *v[2] + m[12],
        m[1]*v[0] + m[5]*v[1] + m[9] *v[2] + m[13],
        m[2]*v[0] + m[6]*v[1] + m[10]*v[2] + m[14]
    };
}

/**
 * @brief 计算两点间距离
 */
inline double distance(const Vec3d& a, const Vec3d& b) {
    double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

} // namespace MathUtils
