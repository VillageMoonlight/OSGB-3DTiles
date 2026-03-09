/**
 * OsgbMetaReader.cpp
 * 自动解析 OSGB 数据目录中的坐标元数据
 *
 * 支持格式：
 *   1. ContextCapture / Smart3D: metadata.xml (<SRSOrigin>)
 *      - 支持 WGS84(EPSG:4326) 和投影坐标系（如 EPSG:4528 CGCS2000）
 *   2. DJI Terra / 大疆智图:     production_meta.xml, Mission.xml
 *   3. Agisoft Metashape:        doc.xml (<reference>)
 */

#include "OsgbMetaReader.h"
#include "Logger.h"

// GDAL 投影转换
#include <ogr_spatialref.h>
#include <ogr_geometry.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

// ────────────────────────────────────────────────────────────────────────────
// 简单 XML 文本提取辅助函数（不引入重型 XML 库）
// ────────────────────────────────────────────────────────────────────────────

/// 从 xmlContent 中提取 <tagName>content</tagName> 的内容（首次出现）
static std::string xmlGetTag(const std::string& xml, const std::string& tag) {
    std::string open  = "<" + tag;   // 允许有属性：<SRSOrigin attr=...>
    std::string close = "</" + tag + ">";
    auto pos = xml.find(open);
    if (pos == std::string::npos) return {};
    // 跳过到 '>'
    auto gt = xml.find('>', pos);
    if (gt == std::string::npos) return {};
    auto end = xml.find(close, gt + 1);
    if (end == std::string::npos) return {};
    return xml.substr(gt + 1, end - gt - 1);
}

/// 从 xmlContent 中提取属性值 attrName="value"
static std::string xmlGetAttr(const std::string& xml,
                              const std::string& tag,
                              const std::string& attr) {
    std::string open = "<" + tag;
    auto pos = xml.find(open);
    if (pos == std::string::npos) return {};
    auto gt = xml.find('>', pos);
    if (gt == std::string::npos) return {};
    std::string elem = xml.substr(pos, gt - pos + 1);

    // 搜索 attr="..." 或 attr='...'
    std::string key = attr + "=";
    auto kpos = elem.find(key);
    if (kpos == std::string::npos) return {};
    kpos += key.size();
    char quote = elem[kpos];
    if (quote != '"' && quote != '\'') return {};
    auto epos = elem.find(quote, kpos + 1);
    if (epos == std::string::npos) return {};
    return elem.substr(kpos + 1, epos - kpos - 1);
}

/// 读取整个文件内容到字符串
static std::string readFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    return {std::istreambuf_iterator<char>(ifs),
            std::istreambuf_iterator<char>()};
}

// ────────────────────────────────────────────────────────────────────────────
// parseSrsOrigin
// ────────────────────────────────────────────────────────────────────────────

bool OsgbMetaReader::parseSrsOrigin(const std::string& s,
                                    double& x, double& y, double& z) {
    // 支持 "x,y,z" 或 "x y z"（逗号或空格分隔）
    std::string tmp = s;
    for (auto& c : tmp) if (c == ',') c = ' ';

    std::istringstream ss(tmp);
    if (!(ss >> x >> y)) return false;
    if (!(ss >> z)) z = 0.0;
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// readContextCapture  (metadata.xml)
// ────────────────────────────────────────────────────────────────────────────
//
// 典型结构：
//   <ModelMetadata version="1">
//     <SRS>EPSG:4326</SRS>
//     <SRSOrigin>120.123456,30.654321,0</SRSOrigin>
//     ...
//   </ModelMetadata>
//
std::optional<OsgbOrigin> OsgbMetaReader::readContextCapture(const std::string& xmlPath) {
    std::string xml = readFile(xmlPath);
    if (xml.empty()) return std::nullopt;

    // 读取 <SRS> 和 <SRSOrigin>
    std::string srsStr    = xmlGetTag(xml, "SRS");
    std::string originStr = xmlGetTag(xml, "SRSOrigin");

    if (originStr.empty()) {
        // 有时用 <Origin> 内子标签
        std::string originBlock = xmlGetTag(xml, "Origin");
        if (!originBlock.empty()) {
            std::string lonStr = xmlGetTag(originBlock, "Longitude");
            std::string latStr = xmlGetTag(originBlock, "Latitude");
            std::string altStr = xmlGetTag(originBlock, "Altitude");
            if (lonStr.empty()) lonStr = xmlGetTag(originBlock, "x");
            if (latStr.empty()) latStr = xmlGetTag(originBlock, "y");
            if (altStr.empty()) altStr = xmlGetTag(originBlock, "z");
            if (!lonStr.empty() && !latStr.empty())
                originStr = lonStr + "," + latStr + "," + altStr;
        }
    }
    if (originStr.empty()) return std::nullopt;

    double x = 0, y = 0, z = 0;
    if (!parseSrsOrigin(originStr, x, y, z)) return std::nullopt;

    OsgbOrigin origin;
    origin.srs        = srsStr;
    origin.sourcefile = xmlPath;

    // 判断是否已经是 WGS84 经纬度
    bool isWgs84 = srsStr.empty()
                || srsStr == "EPSG:4326"
                || srsStr == "WGS84"
                || srsStr == "WGS 84";

    if (isWgs84) {
        // 直接使用（x=lon, y=lat）
        if (x < -180.0 || x > 180.0 || y < -90.0 || y > 90.0) return std::nullopt;
        origin.longitude = x;
        origin.latitude  = y;
        origin.height    = z;
    } else {
        // 需要将投影坐标转换为 WGS84 经纬度
        //
        // 转换顺序：
        //   1. 优先用 GDAL/OGR（通用：支持 UTM、高斯-克吕格、Lambert、CGCS2000、地方坐标等）
        //   2. GDAL 失败（通常因为找不到 proj.db）时，降级到内置高斯-克吕格反算（仅支持 CGCS2000）
        //
        LOG_INFO("SRS is " + srsStr + ", converting to WGS84...");

        OGRSpatialReference srcSRS, dstSRS;
        srcSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dstSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dstSRS.importFromEPSG(4326);

        bool gdalOk = false;

        // ── 方式1：GDAL/PROJ 通用转换（支持任意坐标系）──────────────────
        {
            OGRErr err = srcSRS.SetFromUserInput(srsStr.c_str());
            if (err == OGRERR_NONE) {
                OGRCoordinateTransformation* ct =
                    OGRCreateCoordinateTransformation(&srcSRS, &dstSRS);
                if (ct) {
                    double px = x, py = y, pz = z;
                    if (ct->Transform(1, &px, &py, &pz)) {
                        origin.longitude = px;
                        origin.latitude  = py;
                        origin.height    = pz;
                        gdalOk = true;
                        LOG_INFO("GDAL/PROJ conversion OK: lon=" +
                                 std::to_string(px) + " lat=" + std::to_string(py));
                    } else {
                        LOG_WARN("GDAL Transform() failed for SRS: " + srsStr +
                                 "  (proj.db 可能未找到，尝试 PROJ_DATA 环境变量)");
                    }
                    OGRCoordinateTransformation::DestroyCT(ct);
                } else {
                    LOG_WARN("Cannot create OGRCoordinateTransformation for: " + srsStr);
                }
            } else {
                LOG_WARN("GDAL cannot parse SRS string: " + srsStr);
            }
        }

        // ── 方式2：内置高斯-克吕格反算（兜底，仅限 CGCS2000/北京54/西安80）──
        if (!gdalOk) {
            LOG_INFO("Falling back to built-in Gauss-Kruger inverse calculation...");

            // 自动判断 Easting/Northing：含带号前缀的分量 > 10,000,000
            double easting  = x;
            double northing = y;
            if (y > x && y > 10000000.0) { easting = y; northing = x; }

            int    zoneNum = 0;
            double cm      = 0.0; // 中央子午线（度）

            if (easting > 1000000) {
                zoneNum  = static_cast<int>(easting / 1000000);
                easting -= zoneNum * 1000000.0; // 去掉带号前缀
            }

            LOG_INFO("Extracted: easting=" + std::to_string(easting) +
                     " northing=" + std::to_string(northing) +
                     " zoneNum=" + std::to_string(zoneNum));

            if (zoneNum >= 25 && zoneNum <= 45) {
                cm = zoneNum * 3.0; // CGCS2000 3度带
                LOG_INFO("3-degree Gauss zone " + std::to_string(zoneNum) +
                         ", CM=" + std::to_string(cm) + "°");
            } else if (zoneNum >= 13 && zoneNum <= 24) {
                cm = (zoneNum - 1) * 6.0 + 3.0; // 6度带
                LOG_INFO("6-degree Gauss zone " + std::to_string(zoneNum) +
                         ", CM=" + std::to_string(cm) + "°");
            }

            if (cm > 0.0) {
                const double PI  = 3.14159265358979323846;
                const double a   = 6378137.0;
                const double f   = 1.0 / 298.257222101;
                const double e2  = 2*f - f*f;
                const double ep2 = e2 / (1.0 - e2);

                double N0  = northing;
                double E0  = easting - 500000.0;

                const double A0 = 1.0 - e2/4.0 - 3.0*e2*e2/64.0 - 5.0*e2*e2*e2/256.0;
                const double A2 = 3.0/8.0 * (e2 + e2*e2/4.0 + 15.0*e2*e2*e2/128.0);
                const double A4 = 15.0/256.0 * (e2*e2 + 3.0*e2*e2*e2/4.0);
                const double A6 = 35.0*e2*e2*e2/3072.0;

                double phi = N0 / a;
                for (int it = 0; it < 10; ++it) {
                    double M  = a * (A0*phi - A2*std::sin(2*phi)
                                    + A4*std::sin(4*phi) - A6*std::sin(6*phi));
                    double dM = a * (A0 - 2*A2*std::cos(2*phi)
                                    + 4*A4*std::cos(4*phi) - 6*A6*std::cos(6*phi));
                    phi += (N0 - M) / dM;
                }

                double sn = std::sin(phi), cn = std::cos(phi), tn = std::tan(phi);
                double N  = a / std::sqrt(1.0 - e2*sn*sn);
                double T  = tn*tn, C = ep2*cn*cn;
                double R  = a*(1.0-e2) / std::pow(1.0-e2*sn*sn, 1.5);
                double D  = E0 / N;

                double lat = phi - (N*tn/R)*(D*D/2.0
                             - (5.0+3.0*T+10.0*C-4.0*C*C-9.0*ep2)*D*D*D*D/24.0
                             + (61.0+90.0*T+298.0*C+45.0*T*T-252.0*ep2-3.0*C*C)*D*D*D*D*D*D/720.0);
                double lon = cm*(PI/180.0)
                             + (D-(1.0+2.0*T+C)*D*D*D/6.0
                                +(5.0-2.0*C+28.0*T-3.0*C*C+8.0*ep2+24.0*T*T)*D*D*D*D*D/120.0)/cn;

                origin.longitude = lon * (180.0 / PI);
                origin.latitude  = lat * (180.0 / PI);
                origin.height    = z;
                gdalOk = true;
                LOG_INFO("Built-in GK inverse OK: lon=" + std::to_string(origin.longitude) +
                         " lat=" + std::to_string(origin.latitude));
            }
        }

        if (!gdalOk) {
            LOG_WARN("Cannot convert SRS '" + srsStr + "' to WGS84.\n"
                     "  提示: 请设置 PROJ_DATA 环境变量指向 proj.db 所在目录，\n"
                     "  或将 proj/ 子目录放置在 osgb2tiles.exe 同目录下。");
            return std::nullopt;
        }
    }

    return origin;
}

// ────────────────────────────────────────────────────────────────────────────
// readDjiTerra  (production_meta.xml / Mission.xml / config.xml)
// ────────────────────────────────────────────────────────────────────────────
//
// DJI Terra 典型结构（production_meta.xml）：
//   <Center longitude="120.123456" latitude="30.654321" altitude="50.0" />
//
// 也有 Mission.xml 格式：
//   <MissionConfig>
//     <GimbalPitch>...</GimbalPitch>
//     <FlyPoint lon="120.123456" lat="30.654321" ... />
//   </MissionConfig>
//
std::optional<OsgbOrigin> OsgbMetaReader::readDjiTerra(const std::string& xmlPath) {
    std::string xml = readFile(xmlPath);
    if (xml.empty()) return std::nullopt;

    // 尝试 <Center longitude=... latitude=... altitude=.../>
    {
        std::string lon = xmlGetAttr(xml, "Center", "longitude");
        std::string lat = xmlGetAttr(xml, "Center", "latitude");
        std::string alt = xmlGetAttr(xml, "Center", "altitude");
        if (lon.empty()) {
            // 大疆智图另一种格式：<center_longitude>, <center_latitude>
            lon = xmlGetTag(xml, "center_longitude");
            lat = xmlGetTag(xml, "center_latitude");
            alt = xmlGetTag(xml, "center_altitude");
        }
        if (!lon.empty() && !lat.empty()) {
            OsgbOrigin origin;
            try {
                origin.longitude = std::stod(lon);
                origin.latitude  = std::stod(lat);
                origin.height    = alt.empty() ? 0.0 : std::stod(alt);
            } catch (...) { return std::nullopt; }
            if (origin.longitude < -180 || origin.longitude > 180) return std::nullopt;
            if (origin.latitude  <  -90 || origin.latitude  >  90) return std::nullopt;
            origin.sourcefile = xmlPath;
            return origin;
        }
    }

    // 尝试 <position> 或 <homepoint>
    {
        std::string posBlock = xmlGetTag(xml, "position");
        if (posBlock.empty()) posBlock = xmlGetTag(xml, "homepoint");
        if (!posBlock.empty()) {
            std::string lon = xmlGetTag(posBlock, "longitude");
            std::string lat = xmlGetTag(posBlock, "latitude");
            std::string alt = xmlGetTag(posBlock, "altitude");
            if (!lon.empty() && !lat.empty()) {
                OsgbOrigin origin;
                try {
                    origin.longitude = std::stod(lon);
                    origin.latitude  = std::stod(lat);
                    origin.height    = alt.empty() ? 0.0 : std::stod(alt);
                } catch (...) { return std::nullopt; }
                origin.sourcefile = xmlPath;
                return origin;
            }
        }
    }

    return std::nullopt;
}

// ────────────────────────────────────────────────────────────────────────────
// readMetashape  (doc.xml)
// ────────────────────────────────────────────────────────────────────────────
//
// Agisoft Metashape 典型 doc.xml:
//   <chunk label="..." >
//     <cameras>...</cameras>
//     <reference x="120.123456" y="30.654321" z="100.0" ... />
//   </chunk>
//
std::optional<OsgbOrigin> OsgbMetaReader::readMetashape(const std::string& xmlPath) {
    std::string xml = readFile(xmlPath);
    if (xml.empty()) return std::nullopt;

    std::string lonStr = xmlGetAttr(xml, "reference", "x");
    std::string latStr = xmlGetAttr(xml, "reference", "y");
    std::string altStr = xmlGetAttr(xml, "reference", "z");

    if (lonStr.empty() || latStr.empty()) return std::nullopt;

    OsgbOrigin origin;
    try {
        origin.longitude = std::stod(lonStr);
        origin.latitude  = std::stod(latStr);
        origin.height    = altStr.empty() ? 0.0 : std::stod(altStr);
    } catch (...) { return std::nullopt; }

    if (origin.longitude < -180 || origin.longitude > 180) return std::nullopt;
    if (origin.latitude  <  -90 || origin.latitude  >  90) return std::nullopt;

    origin.sourcefile = xmlPath;
    origin.srs = "EPSG:4326";
    return origin;
}

// ────────────────────────────────────────────────────────────────────────────
// tryRead —— 主入口（按优先级依次尝试）
// ────────────────────────────────────────────────────────────────────────────
std::optional<OsgbOrigin> OsgbMetaReader::tryRead(const std::string& osgbDir) {
    // 候选文件名列表（分组，每组用对应的解析器）
    struct Candidate {
        std::string filename;
        int         parserType; // 1=CC, 2=DJI, 3=Metashape, 0=Auto
    };

    const std::vector<Candidate> candidates = {
        // ContextCapture / Smart3D
        {"metadata.xml",            1},
        {"Metadata.xml",            1},
        {"metadata.XML",            1},
        {"ProductionProperties.xml",1},
        // DJI Terra / 大疆智图
        {"production_meta.xml",     2},
        {"ProductionMeta.xml",      2},
        {"config.xml",              2},
        {"Mission.xml",             2},
        // Agisoft Metashape
        {"doc.xml",                 3},
        // 通用后备
        {"origin.xml",              0},
        {"georef.xml",              0},
    };

    // 搜索路径：osgbDir 本身以及其直接子目录（depth=1）
    std::vector<std::string> searchDirs = { osgbDir };
    try {
        for (const auto& entry : fs::directory_iterator(osgbDir)) {
            if (entry.is_directory()) {
                searchDirs.push_back(entry.path().string());
            }
        }
    } catch (...) {}

    for (const auto& dir : searchDirs) {
        for (const auto& cand : candidates) {
            std::string xmlPath = dir + "/" + cand.filename;

            // 大小写一致性处理（Windows 不区分大小写，Linux 区分）
            if (!fs::exists(xmlPath)) continue;

            LOG_DEBUG("Trying metadata: " + xmlPath);

            std::optional<OsgbOrigin> result;
            switch (cand.parserType) {
                case 1: result = readContextCapture(xmlPath); break;
                case 2: result = readDjiTerra(xmlPath);       break;
                case 3: result = readMetashape(xmlPath);      break;
                default:
                    // 通用：依次尝试所有解析器
                    result = readContextCapture(xmlPath);
                    if (!result) result = readDjiTerra(xmlPath);
                    if (!result) result = readMetashape(xmlPath);
                    break;
            }

            if (result) {
                LOG_INFO("Auto-detected origin from: " + xmlPath);
                LOG_INFO("  lon=" + std::to_string(result->longitude) +
                         " lat=" + std::to_string(result->latitude)  +
                         " alt=" + std::to_string(result->height));
                return result;
            }
        }
    }

    LOG_DEBUG("No metadata file found in: " + osgbDir);
    return std::nullopt;
}
