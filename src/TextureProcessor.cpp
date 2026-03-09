// TextureProcessor 使用 stb_image 和 stb_image_write（header-only）
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include "TextureProcessor.h"
#include "Logger.h"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

// WebP 编码（libwebp via vcpkg）
#include <webp/encode.h>

// KTX2 Basis Universal
#ifdef HAVE_KTX
#include <ktx.h>
// VK_FORMAT constants without requiring full Vulkan SDK
#ifndef VK_FORMAT_R8G8B8_SRGB
#define VK_FORMAT_R8G8B8_SRGB 29u
#endif
#ifndef VK_FORMAT_R8G8B8A8_UNORM
#define VK_FORMAT_R8G8B8A8_UNORM                                               \
  37u // linear RGBA8, required by Basis Universal
#endif
#ifndef VK_FORMAT_R8G8B8A8_SRGB
#define VK_FORMAT_R8G8B8A8_SRGB 43u
#endif
#endif // HAVE_KTX

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

TextureProcessor::TextureProcessor(const ConvertConfig &cfg) : cfg_(cfg) {}

bool TextureProcessor::process(TileNode &node) {
  // ── 处理各 SubMesh 的纹理（多纹理支持）─────────────────────────
  for (auto &sm : node.subMeshes) {
    if (!sm.textureData.empty() && sm.texWidth > 0 && sm.texHeight > 0) {
      // 已有内嵌像素，只需缩放
      if (cfg_.maxTextureSize > 0 && (sm.texWidth > cfg_.maxTextureSize ||
                                      sm.texHeight > cfg_.maxTextureSize)) {
        // 内联缩放
        int mxs = cfg_.maxTextureSize;
        float ratio =
            static_cast<float>(mxs) / std::max(sm.texWidth, sm.texHeight);
        int nw = std::max(1, (int)(sm.texWidth * ratio));
        int nh = std::max(1, (int)(sm.texHeight * ratio));
        std::vector<uint8_t> buf(nw * nh * 3);
        stbir_resize_uint8_linear(sm.textureData.data(), sm.texWidth,
                                  sm.texHeight, 0, buf.data(), nw, nh, 0,
                                  STBIR_RGB);
        sm.textureData = std::move(buf);
        sm.texWidth = nw;
        sm.texHeight = nh;
      }
    } else if (!sm.texturePath.empty()) {
      // 从文件加载
      if (fs::exists(sm.texturePath)) {
        int w, h, c;
        stbi_uc *data = stbi_load(sm.texturePath.c_str(), &w, &h, &c, 3);
        if (data) {
          sm.texWidth = w;
          sm.texHeight = h;
          sm.textureData.assign(data, data + w * h * 3);
          stbi_image_free(data);
          if (cfg_.maxTextureSize > 0 &&
              (w > cfg_.maxTextureSize || h > cfg_.maxTextureSize)) {
            int mxs = cfg_.maxTextureSize;
            float ratio = static_cast<float>(mxs) / std::max(w, h);
            int nw = std::max(1, (int)(w * ratio)),
                nh = std::max(1, (int)(h * ratio));
            std::vector<uint8_t> buf(nw * nh * 3);
            stbir_resize_uint8_linear(sm.textureData.data(), w, h, 0,
                                      buf.data(), nw, nh, 0, STBIR_RGB);
            sm.textureData = std::move(buf);
            sm.texWidth = nw;
            sm.texHeight = nh;
          }
        }
      }
    } else {
      // 无纹理：灰色占位
      sm.texWidth = 4;
      sm.texHeight = 4;
      sm.textureData.assign(4 * 4 * 3, 200);
    }

    // ── 编码 SubMesh 纹理为目标格式 ─────────────────────────────
    if (!sm.textureData.empty() && sm.encodedData.empty()) {
      auto writeFunc = [](void *ctx, void *data, int size) {
        auto *buf = static_cast<std::vector<uint8_t> *>(ctx);
        const uint8_t *p = static_cast<const uint8_t *>(data);
        buf->insert(buf->end(), p, p + size);
      };
      const std::string &fmt = cfg_.textureFormat;
      if (fmt == "png") {
        stbi_write_png_to_func(writeFunc, &sm.encodedData, sm.texWidth,
                               sm.texHeight, 3, sm.textureData.data(),
                               sm.texWidth * 3);
      } else if (fmt == "webp") {
        // WebP 编码（需链接 libwebp）
        uint8_t *output = nullptr;
        size_t outputSize = WebPEncodeRGB(
            sm.textureData.data(), sm.texWidth, sm.texHeight, sm.texWidth * 3,
            static_cast<float>(cfg_.webpQuality), &output);
        if (output && outputSize > 0) {
          sm.encodedData.assign(output, output + outputSize);
          WebPFree(output);
        } else {
          // 退回 JPEG
          stbi_write_jpg_to_func(writeFunc, &sm.encodedData, sm.texWidth,
                                 sm.texHeight, 3, sm.textureData.data(),
                                 cfg_.jpegQuality);
        }
      } else if (fmt == "ktx2") {
#ifdef HAVE_KTX
        sm.encodedData =
            encodeKtx2Raw(sm.texWidth, sm.texHeight, sm.textureData);
        if (sm.encodedData.empty()) {
          LOG_WARN("KTX2 encode failed for SubMesh, falling back to JPEG");
          stbi_write_jpg_to_func(writeFunc, &sm.encodedData, sm.texWidth,
                                 sm.texHeight, 3, sm.textureData.data(),
                                 cfg_.jpegQuality);
        }
#else
        LOG_WARN("KTX2 not available (build without HAVE_KTX), falling back to "
                 "JPEG");
        stbi_write_jpg_to_func(writeFunc, &sm.encodedData, sm.texWidth,
                               sm.texHeight, 3, sm.textureData.data(),
                               cfg_.jpegQuality);
#endif
      } else {
        // 默认 JPEG
        stbi_write_jpg_to_func(writeFunc, &sm.encodedData, sm.texWidth,
                               sm.texHeight, 3, sm.textureData.data(),
                               cfg_.jpegQuality);
      }
    }
  }

  // ── 向后兼容：处理主纹理（无 subMeshes 时使用）──────────────────
  if (!node.textureData.empty() && node.texWidth > 0 && node.texHeight > 0) {
    if (cfg_.maxTextureSize > 0 && (node.texWidth > cfg_.maxTextureSize ||
                                    node.texHeight > cfg_.maxTextureSize)) {
      resize(node, cfg_.maxTextureSize);
    }
    return true;
  }
  // 次之：从外部文件加载
  if (!node.texturePath.empty()) {
    return loadFromFile(node.texturePath, node);
  }
  // 无纹理时生成浅灰色 4x4 像素占位
  node.texWidth = 4;
  node.texHeight = 4;
  node.textureData.assign(4 * 4 * 3, 200);
  return true;
}

bool TextureProcessor::loadFromFile(const std::string &path, TileNode &node) {
  if (!fs::exists(path)) {
    LOG_WARN("Texture not found: " + path);
    return false;
  }
  int w, h, c;
  stbi_uc *data = stbi_load(path.c_str(), &w, &h, &c, 3); // 强制 RGB
  if (!data) {
    LOG_WARN("Failed to load texture: " + path + " (" + stbi_failure_reason() +
             ")");
    return false;
  }

  node.texWidth = w;
  node.texHeight = h;
  node.textureData.assign(data, data + w * h * 3);
  stbi_image_free(data);

  // 缩放到最大允许尺寸
  if (cfg_.maxTextureSize > 0 &&
      (w > cfg_.maxTextureSize || h > cfg_.maxTextureSize)) {
    resize(node, cfg_.maxTextureSize);
  }

  return true;
}

void TextureProcessor::resize(TileNode &node, int maxSize) {
  int newW = node.texWidth, newH = node.texHeight;
  if (newW > maxSize || newH > maxSize) {
    float ratio = static_cast<float>(maxSize) / std::max(newW, newH);
    newW = static_cast<int>(newW * ratio);
    newH = static_cast<int>(newH * ratio);
    newW = std::max(1, newW);
    newH = std::max(1, newH);
  }

  std::vector<uint8_t> resized(newW * newH * 3);
  stbir_resize_uint8_linear(node.textureData.data(), node.texWidth,
                            node.texHeight, 0, resized.data(), newW, newH, 0,
                            STBIR_RGB);

  node.textureData = std::move(resized);
  node.texWidth = newW;
  node.texHeight = newH;
  LOG_DEBUG("Texture resized to " + std::to_string(newW) + "x" +
            std::to_string(newH));
}

// ─── JPEG 编码
// ────────────────────────────────────────────────────────────────
std::vector<uint8_t> TextureProcessor::encodeJpeg(const TileNode &node,
                                                  int quality) {
  std::vector<uint8_t> out;
  auto writeFunc = [](void *ctx, void *data, int size) {
    auto *buf = static_cast<std::vector<uint8_t> *>(ctx);
    const uint8_t *p = static_cast<const uint8_t *>(data);
    buf->insert(buf->end(), p, p + size);
  };
  stbi_write_jpg_to_func(writeFunc, &out, node.texWidth, node.texHeight, 3,
                         node.textureData.data(), quality);
  return out;
}

// ─── PNG 编码
// ─────────────────────────────────────────────────────────────────
std::vector<uint8_t> TextureProcessor::encodePng(const TileNode &node) {
  std::vector<uint8_t> out;
  auto writeFunc = [](void *ctx, void *data, int size) {
    auto *buf = static_cast<std::vector<uint8_t> *>(ctx);
    const uint8_t *p = static_cast<const uint8_t *>(data);
    buf->insert(buf->end(), p, p + size);
  };
  stbi_write_png_to_func(writeFunc, &out, node.texWidth, node.texHeight, 3,
                         node.textureData.data(), node.texWidth * 3);
  return out;
}

// ─── WebP 编码
// ────────────────────────────────────────────────────────────────
std::vector<uint8_t> TextureProcessor::encodeWebp(const TileNode &node,
                                                  int quality, bool lossless) {
  uint8_t *output = nullptr;
  size_t outputSize = 0;

  if (lossless) {
    outputSize =
        WebPEncodeLosslessRGB(node.textureData.data(), node.texWidth,
                              node.texHeight, node.texWidth * 3, &output);
  } else {
    outputSize =
        WebPEncodeRGB(node.textureData.data(), node.texWidth, node.texHeight,
                      node.texWidth * 3, static_cast<float>(quality), &output);
  }

  if (!output || outputSize == 0) {
    LOG_WARN("WebP encoding failed, falling back to JPEG");
    WebPFree(output);
    return encodeJpeg(node, cfg_.jpegQuality);
  }

  std::vector<uint8_t> result(output, output + outputSize);
  WebPFree(output);
  return result;
}

// ─── encode 主入口
// ────────────────────────────────────────────────────────────
std::vector<uint8_t> TextureProcessor::encode(const TileNode &node) {
  const std::string &fmt = cfg_.textureFormat;

  if (fmt == "ktx2") {
    auto out = encodeKtx2Raw(node.texWidth, node.texHeight, node.textureData);
    if (!out.empty())
      return out;
    LOG_WARN("KTX2 encode failed (main path), falling back to JPEG");
  }
  if (fmt == "png") {
    return encodePng(node);
  } else if (fmt == "webp") {
    return encodeWebp(node, cfg_.webpQuality, cfg_.webpLossless);
  } else {
    return encodeJpeg(node, cfg_.jpegQuality);
  }
}

// ─── MIME 类型
// ────────────────────────────────────────────────────────────────
std::string TextureProcessor::mimeType() const {
  const std::string &fmt = cfg_.textureFormat;
  if (fmt == "ktx2")
    return "image/ktx2";
  if (fmt == "png")
    return "image/png";
  if (fmt == "webp")
    return "image/webp";
  return "image/jpeg";
}

// ─── KTX2 Basis Universal 编码
// ──────────────────────────────────────────────── 注意：Basis Universal
// (CompressBasisEx) 要求 4 通道 RGBA 输入。 传入 3 通道 RGB +
// VK_FORMAT_R8G8B8_SRGB 会返回 KTX_INVALID_OPERATION(10)。 解决方案：将 RGB
// 扩展为 RGBA（alpha=255），使用 VK_FORMAT_R8G8B8A8_SRGB。
std::vector<uint8_t>
TextureProcessor::encodeKtx2Raw(int w, int h, const std::vector<uint8_t> &rgb) {
#ifdef HAVE_KTX
  // ── ETC1S 全局串行化锁 ─────────────────────────────────────────
  // BasisU ETC1S 码本训练共享全局状态，并发调用会竞争导致速度退化。
  // UASTC 逐块独立，安全并行，不加锁。
  static std::mutex s_etc1sMutex;
  std::unique_lock<std::mutex> etc1sLock(s_etc1sMutex, std::defer_lock);
  if (cfg_.ktx2Mode != "uastc") {
    etc1sLock.lock(); // ETC1S：全局串行，防止竞争
  }
  // 0. RGB → RGBA（Basis Universal 必须 4 通道）
  std::vector<uint8_t> rgba;
  rgba.reserve(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
    rgba.push_back(rgb[i]);
    rgba.push_back(rgb[i + 1]);
    rgba.push_back(rgb[i + 2]);
    rgba.push_back(255u); // alpha 完全不透明
  }

  // 1. 创建 KTX2 纹理对象
  // 注意：使用 UNORM 而非 SRGB——Basis 编码器在某些版本中会对 SRGB 格式返回
  // KTX_INVALID_OPERATION(10)，UNORM 是 BasisU 压缩的正确线性格式。
  ktxTextureCreateInfo ci{};
  ci.vkFormat = VK_FORMAT_R8G8B8A8_UNORM; // UNORM，不能用 SRGB
  ci.baseWidth = static_cast<ktx_uint32_t>(w);
  ci.baseHeight = static_cast<ktx_uint32_t>(h);
  ci.baseDepth = 1;
  ci.numDimensions = 2;
  ci.numLevels = 1;
  ci.numLayers = 1;
  ci.numFaces = 1;
  ci.isArray = KTX_FALSE;
  // generateMipmaps=KTX_TRUE 与 CompressBasisEx 在 kt v4.x 中会触发
  // KTX_INVALID_OPERATION(10) （mipmap 由渲染器在 GPU 端生成，单 Level KTX2 对
  // 3D Tiles 完全足够）
  ci.generateMipmaps = KTX_FALSE;

  ktxTexture2 *tex = nullptr;
  KTX_error_code ec =
      ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
  if (ec != KTX_SUCCESS || !tex) {
    LOG_WARN("ktxTexture2_Create failed: " + std::to_string(ec));
    return {};
  }

  // 2. 写入 RGBA 像素数据
  ec = ktxTexture_SetImageFromMemory(ktxTexture(tex), 0, 0, 0, rgba.data(),
                                     static_cast<ktx_size_t>(w * h * 4));
  if (ec != KTX_SUCCESS) {
    ktxTexture_Destroy(ktxTexture(tex));
    LOG_WARN("ktxTexture_SetImageFromMemory failed: " + std::to_string(ec));
    return {};
  }

  // 3. Basis 压缩参数
  // 注意：inputSwizzle 必须是合法的 swizzle 字符串（"rgba"）或全 '\0'（忽略）。
  // 零初始化（{}）会产生 \0\0\0\0，部分版本视为非法触发 KTX_INVALID_OPERATION。
  ktxBasisParams bp{};
  bp.structSize = sizeof(bp);
  bp.uastc = (cfg_.ktx2Mode == "uastc") ? KTX_TRUE : KTX_FALSE;
  bp.compressionLevel = static_cast<ktx_uint32_t>(cfg_.ktx2Quality); // 1-5
  bp.qualityLevel = 128; // ETC1S 质量 1-255 (128=default)
  // ETC1S 已被全局 mutex 串行化，单次编码可安全使用所有 CPU 核心。
  // BasisU ETC1S 的 codebook 训练 + 块编码均支持多线程，8核可获 4~6x 加速。
  unsigned int nCPU = std::thread::hardware_concurrency();
  bp.threadCount = (nCPU > 0) ? nCPU : 4;
  // 显式设置合法 swizzle（或留空用 "rgba" 默认）
  bp.inputSwizzle[0] = 'r';
  bp.inputSwizzle[1] = 'g';
  bp.inputSwizzle[2] = 'b';
  bp.inputSwizzle[3] = 'a';

  ec = ktxTexture2_CompressBasisEx(tex, &bp);
  if (ec != KTX_SUCCESS) {
    ktxTexture_Destroy(ktxTexture(tex));
    LOG_WARN("KTX2 CompressBasis failed: " + std::to_string(ec));
    return {};
  }

  // UASTC 超压缩：zstd 大幅缩小磁盘文件体积（Cesium 原生支持，运行时自动解压）
  // ETC1S 已经高度压缩，不需要二次压缩。
  // zstd level 9 是速度/大小的好折中（1=最快，22=最小）
  if (cfg_.ktx2Mode == "uastc") {
    ec = ktxTexture2_DeflateZstd(tex, 9);
    if (ec != KTX_SUCCESS) {
      LOG_WARN("UASTC zstd deflate failed (code " + std::to_string(ec) +
               "), continuing without supercompression");
      // 非致命错误，继续写出原始 UASTC
    }
  }

  // 4. 写到内存 buffer
  ktx_uint8_t *outBuf = nullptr;
  ktx_size_t outSize = 0;
  ec = ktxTexture_WriteToMemory(ktxTexture(tex), &outBuf, &outSize);
  ktxTexture_Destroy(ktxTexture(tex));
  if (ec != KTX_SUCCESS || !outBuf) {
    LOG_WARN("KTX2 WriteToMemory failed: " + std::to_string(ec));
    return {};
  }

  std::vector<uint8_t> result(outBuf, outBuf + outSize);
  free(outBuf);
  LOG_DEBUG("KTX2 " + std::to_string(w) + "x" + std::to_string(h) + " -> " +
            std::to_string(outSize) + "B [" + cfg_.ktx2Mode + " q" +
            std::to_string(cfg_.ktx2Quality) +
            (cfg_.ktx2Mipmaps ? " +mip" : "") + "]");
  return result;
#else
  (void)w;
  (void)h;
  (void)rgb;
  return {}; // 未编译 HAVE_KTX
#endif
}
