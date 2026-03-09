#pragma once

#include "OsgbReader.h"
#include "Config.h"
#include <string>
#include <vector>

/**
 * @brief 纹理读取、缩放与格式转换
 * 支持输出格式：JPEG / PNG / WebP（lossy 或 lossless）
 */
class TextureProcessor {
public:
    explicit TextureProcessor(const ConvertConfig& cfg);

    /**
     * @brief 加载并处理纹理到 node.textureData / texWidth / texHeight
     * 支持输入: JPEG / PNG / BMP / TGA（stb_image）
     * 可选：缩放到 maxTextureSize
     */
    bool process(TileNode& node);

    /**
     * @brief 将像素数据压缩为目标格式
     * @return 压缩后的字节流（JPEG / PNG / WebP）
     */
    std::vector<uint8_t> encode(const TileNode& node);

    /**
     * @brief 返回当前配置对应的 MIME 类型字符串
     * "image/jpeg" / "image/png" / "image/webp"
     */
    std::string mimeType() const;

private:
    ConvertConfig cfg_;

    bool loadFromFile(const std::string& path, TileNode& node);
    void resize(TileNode& node, int maxSize);

    std::vector<uint8_t> encodeJpeg(const TileNode& node, int quality);
    std::vector<uint8_t> encodePng(const TileNode& node);
    std::vector<uint8_t> encodeWebp(const TileNode& node, int quality, bool lossless);

    // KTX2 Basis Universal（需链接 libktx，#ifdef HAVE_KTX）
    std::vector<uint8_t> encodeKtx2Raw(int w, int h, const std::vector<uint8_t>& rgb);
};
