#pragma once

#include "OsgbReader.h"
#include "Config.h"
#include <string>
#include <vector>

/**
 * @brief 将处理后的 TileNode 数据打包为 GLB 或 B3DM 文件
 *
 * GLB 格式：标准 gITF Binary（.glb），Cesium 1.0+ 直接支持
 * B3DM 格式：Batched 3D Model（.b3dm），含 FeatureTable / BatchTable 头
 */
class GlbWriter {
public:
    explicit GlbWriter(const ConvertConfig& cfg);

    /**
     * @brief 将 TileNode 写入 GLB 文件
     * @param node     已完成坐标变换的瓦片节点
     * @param texData  纹理字节流（JPEG/WebP）
     * @param mimeType 纹理 MIME 类型
     * @param outPath  输出文件路径（.glb 或 .b3dm）
     * @return true if success
     */
    bool write(const TileNode& node,
               const std::vector<uint8_t>& texData,
               const std::string& mimeType,
               const std::string& outPath);

private:
    ConvertConfig cfg_;

    /// 写入纯 GLB 格式
    bool writeGlb(const TileNode& node,
                  const std::vector<uint8_t>& texData,
                  const std::string& mimeType,
                  const std::string& outPath);

    /// 在 GLB 基础上加 B3DM 头
    bool writeB3dm(const TileNode& node,
                   const std::vector<uint8_t>& texData,
                   const std::string& mimeType,
                   const std::string& outPath);

    /// 写入字节到文件
    static bool writeBytes(const std::string& path, const std::vector<uint8_t>& data);

    /// 4字节对齐填充
    static void pad4(std::vector<uint8_t>& buf, uint8_t padByte = 0x20);
};
