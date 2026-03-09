#pragma once

#include <string>
#include <optional>

/**
 * @brief OSGB 数据集元数据：地理原点坐标（WGS84）
 */
struct OsgbOrigin {
    double longitude = 0.0;  ///< 经度（度）
    double latitude  = 0.0;  ///< 纬度（度）
    double height    = 0.0;  ///< 高程（米，WGS84 椭球面以上）
    std::string sourcefile;  ///< 读取来源文件（便于日志）
    std::string srs;         ///< 坐标参考系名称（如 EPSG:4326）
};

/**
 * @brief 自动从 OSGB 数据目录读取地理原点坐标
 *
 * 支持以下格式（按优先级）：
 *  1. ContextCapture / Smart3D  → metadata.xml  (<SRSOrigin>)
 *  2. DJI Terra / 大疆智图      → production_meta.xml / config.xml
 *  3. Agisoft Metashape         → doc.xml  (<reference>)
 *  4. 通用后备                  → 扫描目录内所有 .xml，查找坐标字段
 */
class OsgbMetaReader {
public:
    /**
     * @brief 尝试从 osgbDir 目录中解析地理原点坐标
     * @param osgbDir  OSGB 数据根目录
     * @return 如果成功解析则返回 OsgbOrigin，否则返回 std::nullopt
     */
    static std::optional<OsgbOrigin> tryRead(const std::string& osgbDir);

private:
    /// ContextCapture / Smart3D: metadata.xml
    static std::optional<OsgbOrigin> readContextCapture(const std::string& xmlPath);

    /// DJI Terra: production_meta.xml、config.xml 等
    static std::optional<OsgbOrigin> readDjiTerra(const std::string& xmlPath);

    /// Agisoft Metashape: doc.xml
    static std::optional<OsgbOrigin> readMetashape(const std::string& xmlPath);

    /// 解析 SRSOrigin 字符串 "lon,lat,alt" 或 "lon lat alt"
    static bool parseSrsOrigin(const std::string& s,
                               double& lon, double& lat, double& alt);
};
