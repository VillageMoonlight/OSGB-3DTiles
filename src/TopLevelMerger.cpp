#include "TopLevelMerger.h"
#include "Logger.h"

// tinygltf — 已在 GlbWriter.cpp 中 TINYGLTF_IMPLEMENTATION，此处只 include
#include <stb_image.h>
#include <tiny_gltf.h>


#include <meshoptimizer.h>
#include <nlohmann/json.hpp>
#include <webp/encode.h>


#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ════════════════════════════════════════════════════════════════════════════
// Step 1: 构建不规则二分合并树
// ════════════════════════════════════════════════════════════════════════════

MergeTreeNode
TopLevelMerger::buildMergeTree(const std::vector<MergeBlockInfo> &blocks,
                               int maxLeafBlocks, int maxDepth) {
  MergeTreeNode root;
  root.name = "Top";
  root.blockIndices.resize(blocks.size());
  std::iota(root.blockIndices.begin(), root.blockIndices.end(), 0);

  // 计算全局包围盒
  for (int i : root.blockIndices) {
    if (blocks[i].bbox.valid()) {
      root.bbox.expandBy(blocks[i].bbox._min);
      root.bbox.expandBy(blocks[i].bbox._max);
    }
  }

  splitNode(root, blocks, "Top", 0, maxLeafBlocks, maxDepth);
  return root;
}

void TopLevelMerger::splitNode(MergeTreeNode &node,
                               const std::vector<MergeBlockInfo> &blocks,
                               const std::string &prefix, int depth,
                               int maxLeafBlocks, int maxDepth) {
  // 终止条件：块数足够少 或 达到最大深度
  if ((int)node.blockIndices.size() <= maxLeafBlocks || depth >= maxDepth) {
    return;
  }

  // 按包围盒长轴二分
  float xSpan = node.bbox._max.x() - node.bbox._min.x();
  float ySpan = node.bbox._max.y() - node.bbox._min.y();
  bool splitX = xSpan >= ySpan;

  // 计算各 block 中心沿分割轴的坐标
  struct CenterIdx {
    float center;
    int idx;
  };
  std::vector<CenterIdx> sorted;
  for (int bi : node.blockIndices) {
    float c =
        splitX ? (blocks[bi].bbox._min.x() + blocks[bi].bbox._max.x()) * 0.5f
               : (blocks[bi].bbox._min.y() + blocks[bi].bbox._max.y()) * 0.5f;
    sorted.push_back({c, bi});
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const CenterIdx &a, const CenterIdx &b) {
              return a.center < b.center;
            });

  // 中点分割
  size_t mid = sorted.size() / 2;
  if (mid == 0)
    mid = 1;

  // 使用字母后缀命名子节点，避免重名
  static const char suffixes[] = {'A', 'B', 'C', 'D'};

  // 如果分割后两组大小差异过大，尝试均分
  std::vector<std::vector<int>> groups(2);
  for (size_t i = 0; i < sorted.size(); ++i) {
    groups[i < mid ? 0 : 1].push_back(sorted[i].idx);
  }

  // 检查是否需要进一步将大的组继续拆分为 3~4 组
  // 策略：如果总数 > 2*maxLeafBlocks，尝试拆为 3 组
  int numGroups = 2;
  if ((int)sorted.size() > 2 * maxLeafBlocks && depth == 0) {
    // 第一层尝试三分
    numGroups = 3;
    groups.resize(3);
    groups[0].clear();
    groups[1].clear();
    groups[2].clear();
    size_t third = sorted.size() / 3;
    for (size_t i = 0; i < sorted.size(); ++i) {
      int g = (i < third) ? 0 : (i < 2 * third) ? 1 : 2;
      groups[g].push_back(sorted[i].idx);
    }
  }

  // 创建子节点
  for (int g = 0; g < numGroups; ++g) {
    if (groups[g].empty())
      continue;

    MergeTreeNode child;
    child.name = prefix + suffixes[g];
    child.blockIndices = groups[g];

    // 计算子节点包围盒
    for (int bi : child.blockIndices) {
      if (blocks[bi].bbox.valid()) {
        child.bbox.expandBy(blocks[bi].bbox._min);
        child.bbox.expandBy(blocks[bi].bbox._max);
      }
    }

    // 递归
    splitNode(child, blocks, child.name, depth + 1, maxLeafBlocks, maxDepth);
    node.children.push_back(std::move(child));
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Step 2: 几何提取 + 合并 + Atlas + 简化
// ════════════════════════════════════════════════════════════════════════════

bool TopLevelMerger::extractFromTile(const std::string &tilePath,
                                     ExtractedBlock &out) {
  // 读取文件
  std::ifstream ifs(tilePath, std::ios::binary);
  if (!ifs) {
    LOG_WARN("Cannot open tile for merge: " + tilePath);
    return false;
  }
  std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
  ifs.close();

  // 判断是 b3dm 还是 glb
  const uint8_t *ptr = fileData.data();
  size_t len = fileData.size();

  // b3dm: magic = 'b3dm', version = 1, 28-byte header
  bool isB3dm = (len >= 28 && ptr[0] == 'b' && ptr[1] == '3' && ptr[2] == 'd' &&
                 ptr[3] == 'm');

  size_t glbOffset = 0;
  if (isB3dm) {
    uint32_t ftJsonLen, ftBinLen, btJsonLen, btBinLen;
    std::memcpy(&ftJsonLen, ptr + 12, 4);
    std::memcpy(&ftBinLen, ptr + 16, 4);
    std::memcpy(&btJsonLen, ptr + 20, 4);
    std::memcpy(&btBinLen, ptr + 24, 4);
    glbOffset = 28 + ftJsonLen + ftBinLen + btJsonLen + btBinLen;
  }

  if (glbOffset >= len) {
    LOG_WARN("Invalid b3dm header in: " + tilePath);
    return false;
  }

  // 使用 tinygltf 解析 GLB
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err, warn;

  bool ok =
      loader.LoadBinaryFromMemory(&model, &err, &warn, ptr + glbOffset,
                                  static_cast<unsigned int>(len - glbOffset));

  if (!ok) {
    LOG_WARN("Failed to parse glb in " + tilePath + ": " + err);
    return false;
  }

  // 提取第一个 mesh 的第一个 primitive
  if (model.meshes.empty())
    return false;

  for (const auto &prim : model.meshes[0].primitives) {
    if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
      continue;

    // 获取 buffer 数据的 lambda
    auto getAccessorData =
        [&](int accIdx) -> std::pair<const uint8_t *, size_t> {
      if (accIdx < 0 || accIdx >= (int)model.accessors.size())
        return {nullptr, 0};
      const auto &acc = model.accessors[accIdx];
      if (acc.bufferView < 0 || acc.bufferView >= (int)model.bufferViews.size())
        return {nullptr, 0};
      const auto &bv = model.bufferViews[acc.bufferView];
      const auto &buf = model.buffers[bv.buffer];
      return {buf.data.data() + bv.byteOffset + acc.byteOffset, acc.count};
    };

    // POSITION
    auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end())
      continue;
    auto [posPtr, posCount] = getAccessorData(posIt->second);
    if (!posPtr || posCount == 0)
      continue;

    size_t baseVtx = out.vertices.size() / 3;
    const float *posF = reinterpret_cast<const float *>(posPtr);
    for (size_t i = 0; i < posCount * 3; ++i)
      out.vertices.push_back(posF[i]);

    // NORMAL
    auto nrmIt = prim.attributes.find("NORMAL");
    if (nrmIt != prim.attributes.end()) {
      auto [nrmPtr, nrmCount] = getAccessorData(nrmIt->second);
      if (nrmPtr) {
        const float *nrmF = reinterpret_cast<const float *>(nrmPtr);
        for (size_t i = 0; i < nrmCount * 3; ++i)
          out.normals.push_back(nrmF[i]);
      }
    }

    // TEXCOORD_0
    auto uvIt = prim.attributes.find("TEXCOORD_0");
    if (uvIt != prim.attributes.end()) {
      auto [uvPtr, uvCount] = getAccessorData(uvIt->second);
      if (uvPtr) {
        const float *uvF = reinterpret_cast<const float *>(uvPtr);
        for (size_t i = 0; i < uvCount * 2; ++i)
          out.uvs.push_back(uvF[i]);
      }
    }

    // Indices
    if (prim.indices >= 0 && prim.indices < (int)model.accessors.size()) {
      const auto &idxAcc = model.accessors[prim.indices];
      if (idxAcc.bufferView >= 0) {
        const auto &bv = model.bufferViews[idxAcc.bufferView];
        const auto &buf = model.buffers[bv.buffer];
        const uint8_t *idxPtr =
            buf.data.data() + bv.byteOffset + idxAcc.byteOffset;

        for (size_t i = 0; i < idxAcc.count; ++i) {
          uint32_t idx = 0;
          if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
            idx = reinterpret_cast<const uint32_t *>(idxPtr)[i];
          } else if (idxAcc.componentType ==
                     TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            idx = reinterpret_cast<const uint16_t *>(idxPtr)[i];
          } else if (idxAcc.componentType ==
                     TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            idx = idxPtr[i];
          }
          out.indices.push_back(static_cast<uint32_t>(baseVtx + idx));
        }
      }
    }

    // 只取第一个 primitive（对应第一个材质/纹理）
    // 提取纹理
    if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
      const auto &mat = model.materials[prim.material];
      int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
      if (texIdx >= 0 && texIdx < (int)model.textures.size()) {
        int imgIdx = model.textures[texIdx].source;
        // 如果 source == -1, 检查 KHR_texture_basisu extension
        if (imgIdx < 0) {
          auto extIt =
              model.textures[texIdx].extensions.find("KHR_texture_basisu");
          if (extIt != model.textures[texIdx].extensions.end()) {
            if (extIt->second.Has("source"))
              imgIdx = extIt->second.Get("source").GetNumberAsInt();
          }
        }
        if (imgIdx >= 0 && imgIdx < (int)model.images.size()) {
          const auto &img = model.images[imgIdx];
          if (!img.image.empty() && img.width > 0 && img.height > 0) {
            // 已解码的像素（tinygltf 自动解码 JPEG/PNG）
            out.texWidth = img.width;
            out.texHeight = img.height;
            // 转为 RGB（tinygltf 可能给 RGBA）
            if (img.component == 4) {
              out.texRGB.resize(img.width * img.height * 3);
              for (int p = 0; p < img.width * img.height; ++p) {
                out.texRGB[p * 3 + 0] = img.image[p * 4 + 0];
                out.texRGB[p * 3 + 1] = img.image[p * 4 + 1];
                out.texRGB[p * 3 + 2] = img.image[p * 4 + 2];
              }
            } else if (img.component == 3) {
              out.texRGB = img.image;
            }
          } else if (img.bufferView >= 0) {
            // 纹理作为编码数据嵌入 buffer（WebP/JPEG/KTX2）
            // 使用 stb_image 解码
            const auto &bv = model.bufferViews[img.bufferView];
            const auto &buf = model.buffers[bv.buffer];
            int w, h, ch;
            uint8_t *decoded = stbi_load_from_memory(
                buf.data.data() + bv.byteOffset,
                static_cast<int>(bv.byteLength), &w, &h, &ch, 3);
            if (decoded) {
              out.texWidth = w;
              out.texHeight = h;
              out.texRGB.assign(decoded, decoded + w * h * 3);
              stbi_image_free(decoded);
            }
          }
        }
      }
    }
    break; // 只处理第一个 primitive
  }

  return !out.vertices.empty() && !out.indices.empty();
}

void TopLevelMerger::buildAtlas(const std::vector<ExtractedBlock> &extracted,
                                std::vector<uint8_t> &atlasRGB, int &atlasW,
                                int &atlasH,
                                std::vector<std::array<float, 4>> &uvRegions) {
  // 简单 strip 排列：所有纹理缩小到统一 lowRes，水平排列
  const int lowRes = 128; // 每个 block 纹理缩到 128x128

  int count = 0;
  for (const auto &eb : extracted) {
    if (eb.texWidth > 0 && eb.texHeight > 0)
      ++count;
  }
  if (count == 0) {
    atlasW = atlasH = 0;
    return;
  }

  // 计算 atlas 布局：ceil(sqrt(count)) × ceil(sqrt(count)) 网格
  int gridSize = (int)std::ceil(std::sqrt((double)count));
  atlasW = gridSize * lowRes;
  atlasH = gridSize * lowRes;
  atlasRGB.resize(atlasW * atlasH * 3, 128); // 灰色背景

  uvRegions.resize(extracted.size(), {0, 0, 1, 1});

  int slot = 0;
  for (size_t i = 0; i < extracted.size(); ++i) {
    const auto &eb = extracted[i];
    if (eb.texWidth <= 0 || eb.texHeight <= 0)
      continue;

    int gx = slot % gridSize;
    int gy = slot / gridSize;
    int ox = gx * lowRes;
    int oy = gy * lowRes;

    // 缩放纹理到 lowRes×lowRes（简单双线性采样）
    for (int y = 0; y < lowRes; ++y) {
      for (int x = 0; x < lowRes; ++x) {
        float sx = (float)x / lowRes * eb.texWidth;
        float sy = (float)y / lowRes * eb.texHeight;
        int srcX = std::min((int)sx, eb.texWidth - 1);
        int srcY = std::min((int)sy, eb.texHeight - 1);
        int srcIdx = (srcY * eb.texWidth + srcX) * 3;
        int dstIdx = ((oy + y) * atlasW + (ox + x)) * 3;
        if (srcIdx + 2 < (int)eb.texRGB.size() &&
            dstIdx + 2 < (int)atlasRGB.size()) {
          atlasRGB[dstIdx + 0] = eb.texRGB[srcIdx + 0];
          atlasRGB[dstIdx + 1] = eb.texRGB[srcIdx + 1];
          atlasRGB[dstIdx + 2] = eb.texRGB[srcIdx + 2];
        }
      }
    }

    // UV 区域: [u0, v0, u1, v1] 在 atlas 中的归一化坐标
    uvRegions[i] = {(float)ox / atlasW, (float)oy / atlasH,
                    (float)(ox + lowRes) / atlasW,
                    (float)(oy + lowRes) / atlasH};

    ++slot;
  }
}

TopLevelMerger::MergedMesh
TopLevelMerger::mergeBlocks(const std::vector<MergeBlockInfo> &blocks,
                            const std::vector<int> &blockIndices,
                            float simplifyRatio) {
  MergedMesh result;

  // 1. 提取所有相关 Block 的几何数据
  std::vector<ExtractedBlock> extracted(blockIndices.size());
  std::vector<bool> extractOk(blockIndices.size(), false);

  for (size_t i = 0; i < blockIndices.size(); ++i) {
    const auto &bi = blocks[blockIndices[i]];
    if (!bi.rootTilePath.empty() && fs::exists(bi.rootTilePath)) {
      extractOk[i] = extractFromTile(bi.rootTilePath, extracted[i]);
    }
  }

  // 2. 构建纹理 Atlas
  std::vector<std::array<float, 4>> uvRegions;
  buildAtlas(extracted, result.atlasRGB, result.atlasWidth, result.atlasHeight,
             uvRegions);

  // 3. 合并几何数据，重映射 UV 到 atlas 坐标
  for (size_t i = 0; i < blockIndices.size(); ++i) {
    if (!extractOk[i])
      continue;
    const auto &eb = extracted[i];
    uint32_t baseVtx = static_cast<uint32_t>(result.vertices.size() / 3);

    // 顶点
    result.vertices.insert(result.vertices.end(), eb.vertices.begin(),
                           eb.vertices.end());

    // 法线
    if (!eb.normals.empty()) {
      result.normals.insert(result.normals.end(), eb.normals.begin(),
                            eb.normals.end());
    }

    // UV 重映射到 atlas 区域
    const auto &region = uvRegions[i];
    float u0 = region[0], v0 = region[1];
    float uSpan = region[2] - region[0];
    float vSpan = region[3] - region[1];

    for (size_t j = 0; j < eb.uvs.size(); j += 2) {
      float u = eb.uvs[j];
      float v = eb.uvs[j + 1];
      // 将原始 UV [0,1] 映射到 atlas 子区域
      u = u0 + std::clamp(u, 0.0f, 1.0f) * uSpan;
      v = v0 + std::clamp(v, 0.0f, 1.0f) * vSpan;
      result.uvs.push_back(u);
      result.uvs.push_back(v);
    }

    // 索引（偏移 baseVtx）
    for (uint32_t idx : eb.indices) {
      result.indices.push_back(baseVtx + idx);
    }
  }

  // 4. 激进简化
  if (!result.vertices.empty() && !result.indices.empty()) {
    simplifyMesh(result, simplifyRatio);
  }

  return result;
}

void TopLevelMerger::simplifyMesh(MergedMesh &mesh, float targetRatio) {
  size_t vtxCount = mesh.vertices.size() / 3;
  size_t idxCount = mesh.indices.size();
  if (idxCount < 9)
    return;

  size_t targetIdxCount =
      static_cast<size_t>(idxCount *
                          std::max(0.01f, std::min(1.0f, targetRatio)) / 3) *
      3;
  if (targetIdxCount >= idxCount)
    return;

  LOG_DEBUG("TopMerge simplify: " + std::to_string(idxCount / 3) + " -> ~" +
            std::to_string(targetIdxCount / 3) + " tris");

  std::vector<uint32_t> simplified(idxCount);
  size_t resultCount = meshopt_simplify(
      simplified.data(), mesh.indices.data(), idxCount, mesh.vertices.data(),
      vtxCount, sizeof(float) * 3, targetIdxCount, 0.02f);

  simplified.resize(resultCount);

  // 重建顶点数组
  std::vector<uint32_t> remap(vtxCount);
  size_t newVtxCount = meshopt_generateVertexRemap(
      remap.data(), simplified.data(), resultCount, mesh.vertices.data(),
      vtxCount, sizeof(float) * 3);

  std::vector<float> newVerts(newVtxCount * 3);
  meshopt_remapVertexBuffer(newVerts.data(), mesh.vertices.data(), vtxCount,
                            sizeof(float) * 3, remap.data());

  std::vector<float> newNorms;
  if (!mesh.normals.empty() && mesh.normals.size() == vtxCount * 3) {
    newNorms.resize(newVtxCount * 3);
    meshopt_remapVertexBuffer(newNorms.data(), mesh.normals.data(), vtxCount,
                              sizeof(float) * 3, remap.data());
  }

  std::vector<float> newUVs;
  if (!mesh.uvs.empty() && mesh.uvs.size() == vtxCount * 2) {
    newUVs.resize(newVtxCount * 2);
    meshopt_remapVertexBuffer(newUVs.data(), mesh.uvs.data(), vtxCount,
                              sizeof(float) * 2, remap.data());
  }

  std::vector<uint32_t> newIdx(resultCount);
  meshopt_remapIndexBuffer(newIdx.data(), simplified.data(), resultCount,
                           remap.data());

  meshopt_optimizeVertexCache(newIdx.data(), newIdx.data(), resultCount,
                              newVtxCount);

  mesh.vertices = std::move(newVerts);
  mesh.normals = std::move(newNorms);
  mesh.uvs = std::move(newUVs);
  mesh.indices = std::move(newIdx);

  LOG_DEBUG("  -> " + std::to_string(resultCount / 3) + " tris after simplify");
}

// ════════════════════════════════════════════════════════════════════════════
// Step 3: 写出合并瓦片 + tileset.json
// ════════════════════════════════════════════════════════════════════════════

bool TopLevelMerger::writeMergedTile(const MergedMesh &mesh,
                                     const std::string &outPath,
                                     const ConvertConfig &cfg) {
  // 将 atlas 编码为 WebP
  std::vector<uint8_t> texEncoded;
  std::string mimeType = "image/webp";

  if (mesh.atlasWidth > 0 && mesh.atlasHeight > 0 && !mesh.atlasRGB.empty()) {
    uint8_t *webpOut = nullptr;
    size_t webpSize =
        WebPEncodeRGB(mesh.atlasRGB.data(), mesh.atlasWidth, mesh.atlasHeight,
                      mesh.atlasWidth * 3, 60, &webpOut); // 低质量，粗模够用
    if (webpOut && webpSize > 0) {
      texEncoded.assign(webpOut, webpOut + webpSize);
      WebPFree(webpOut);
    }
  }

  // 构造 TileNode 给 GlbWriter
  // 这里不直接用 GlbWriter（避免依赖 OSG），自行用 tinygltf 写
  tinygltf::Model model;
  tinygltf::Scene scene;
  tinygltf::Buffer buf;

  model.extensionsUsed.push_back("KHR_materials_unlit");

  // 辅助：添加 BufferView
  auto addBV = [&](const void *data, size_t byteLen, int target) -> int {
    while (buf.data.size() % 4 != 0)
      buf.data.push_back(0);
    size_t offset = buf.data.size();
    buf.data.resize(offset + byteLen);
    if (data && byteLen > 0)
      std::memcpy(buf.data.data() + offset, data, byteLen);
    int idx = static_cast<int>(model.bufferViews.size());
    tinygltf::BufferView bv;
    bv.buffer = 0;
    bv.byteOffset = static_cast<int>(offset);
    bv.byteLength = static_cast<int>(byteLen);
    bv.target = target;
    model.bufferViews.push_back(std::move(bv));
    return idx;
  };

  size_t vtxCount = mesh.vertices.size() / 3;

  // AABB
  std::vector<double> posMin = {1e18, 1e18, 1e18},
                      posMax = {-1e18, -1e18, -1e18};
  for (size_t i = 0; i < vtxCount; ++i) {
    for (int k = 0; k < 3; ++k) {
      double v = mesh.vertices[i * 3 + k];
      posMin[k] = std::min(posMin[k], v);
      posMax[k] = std::max(posMax[k], v);
    }
  }

  // 纹理
  int texIdx = -1;
  if (!texEncoded.empty()) {
    int bvImg = addBV(texEncoded.data(), texEncoded.size(), 0);

    tinygltf::Image img;
    img.bufferView = bvImg;
    img.mimeType = mimeType;
    model.images.push_back(std::move(img));

    tinygltf::Sampler smp;
    smp.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    smp.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    smp.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
    smp.wrapT = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
    model.samplers.push_back(std::move(smp));

    tinygltf::Texture tex;
    tex.sampler = 0;
    tex.source = 0;
    model.textures.push_back(std::move(tex));
    texIdx = 0;
  }

  // Material
  tinygltf::Material mat;
  mat.name = "topMat";
  mat.extensions["KHR_materials_unlit"] =
      tinygltf::Value(tinygltf::Value::Object());
  mat.pbrMetallicRoughness.metallicFactor = 0.0;
  mat.pbrMetallicRoughness.roughnessFactor = 0.5;
  mat.pbrMetallicRoughness.baseColorFactor = {1.0, 1.0, 1.0, 1.0};
  if (texIdx >= 0) {
    mat.pbrMetallicRoughness.baseColorTexture.index = texIdx;
    mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
  }
  mat.doubleSided = true;
  model.materials.push_back(std::move(mat));

  // Primitive
  tinygltf::Primitive prim;
  prim.material = 0;
  prim.mode = TINYGLTF_MODE_TRIANGLES;

  int bvVtx = addBV(mesh.vertices.data(), mesh.vertices.size() * 4,
                    TINYGLTF_TARGET_ARRAY_BUFFER);
  int aVtx = static_cast<int>(model.accessors.size());
  {
    tinygltf::Accessor a;
    a.bufferView = bvVtx;
    a.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    a.type = TINYGLTF_TYPE_VEC3;
    a.count = static_cast<int>(vtxCount);
    a.minValues = posMin;
    a.maxValues = posMax;
    model.accessors.push_back(std::move(a));
  }
  prim.attributes["POSITION"] = aVtx;

  if (!mesh.uvs.empty()) {
    int bvUV = addBV(mesh.uvs.data(), mesh.uvs.size() * 4,
                     TINYGLTF_TARGET_ARRAY_BUFFER);
    int aUV = static_cast<int>(model.accessors.size());
    tinygltf::Accessor a;
    a.bufferView = bvUV;
    a.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    a.type = TINYGLTF_TYPE_VEC2;
    a.count = static_cast<int>(vtxCount);
    model.accessors.push_back(std::move(a));
    prim.attributes["TEXCOORD_0"] = aUV;
  }

  int bvIdx = addBV(mesh.indices.data(), mesh.indices.size() * 4,
                    TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
  int aIdx = static_cast<int>(model.accessors.size());
  {
    tinygltf::Accessor a;
    a.bufferView = bvIdx;
    a.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    a.type = TINYGLTF_TYPE_SCALAR;
    a.count = static_cast<int>(mesh.indices.size());
    model.accessors.push_back(std::move(a));
  }
  prim.indices = aIdx;

  tinygltf::Mesh gltfMesh;
  gltfMesh.name = fs::path(outPath).stem().string();
  gltfMesh.primitives.push_back(std::move(prim));
  model.meshes.push_back(std::move(gltfMesh));

  model.buffers.push_back(std::move(buf));

  tinygltf::Node node;
  node.mesh = 0;
  model.nodes.push_back(std::move(node));
  scene.nodes.push_back(0);
  model.scenes.push_back(std::move(scene));
  model.defaultScene = 0;

  model.asset.version = "2.0";
  model.asset.generator = "osgb2tiles TopMerger";

  // 判断输出格式
  std::string ext = fs::path(outPath).extension().string();
  bool isB3dm = (ext == ".b3dm");

  fs::create_directories(fs::path(outPath).parent_path());

  if (!isB3dm) {
    // 直接写 GLB
    tinygltf::TinyGLTF gltf;
    return gltf.WriteGltfSceneToFile(&model, outPath, true, true, false, true);
  }

  // 写 B3DM：先生成 GLB，再加 B3DM 头
  std::string tmpGlb = outPath + ".tmp.glb";
  {
    tinygltf::TinyGLTF gltf;
    if (!gltf.WriteGltfSceneToFile(&model, tmpGlb, true, true, false, true))
      return false;
  }

  std::ifstream in(tmpGlb, std::ios::binary);
  std::vector<uint8_t> glbData((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
  in.close();
  fs::remove(tmpGlb);

  // B3DM 头
  json ftJson;
  ftJson["BATCH_LENGTH"] = 0;
  std::string ftStr = ftJson.dump();
  while (ftStr.size() % 8 != 0)
    ftStr += ' ';
  std::string btStr = "{}";
  while (btStr.size() % 8 != 0)
    btStr += ' ';

  uint32_t ftJsonLen = static_cast<uint32_t>(ftStr.size());
  uint32_t totalLen = 28 + ftJsonLen + 0 + static_cast<uint32_t>(btStr.size()) +
                      0 + static_cast<uint32_t>(glbData.size());

  std::vector<uint8_t> b3dm;
  b3dm.reserve(totalLen);
  auto appendU32 = [&](uint32_t v) {
    b3dm.insert(b3dm.end(), (uint8_t *)&v, (uint8_t *)&v + 4);
  };
  b3dm.insert(b3dm.end(), {'b', '3', 'd', 'm'});
  appendU32(1); // version
  appendU32(totalLen);
  appendU32(ftJsonLen);
  appendU32(0); // featureTableBinaryByteLength
  appendU32(static_cast<uint32_t>(btStr.size()));
  appendU32(0); // batchTableBinaryByteLength
  b3dm.insert(b3dm.end(), ftStr.begin(), ftStr.end());
  b3dm.insert(b3dm.end(), btStr.begin(), btStr.end());
  b3dm.insert(b3dm.end(), glbData.begin(), glbData.end());

  std::ofstream ofs(outPath, std::ios::binary);
  if (!ofs)
    return false;
  ofs.write(reinterpret_cast<const char *>(b3dm.data()),
            static_cast<std::streamsize>(b3dm.size()));
  return ofs.good();
}

// ── tileset.json 生成 ──────────────────────────────────────────────────

json TopLevelMerger::buildTopTilesetNode(
    const MergeTreeNode &node, const std::vector<MergeBlockInfo> &blocks) {
  json jnode;

  // 包围盒
  if (node.bbox.valid()) {
    float cx = (node.bbox._min.x() + node.bbox._max.x()) * 0.5f;
    float cy = (node.bbox._min.y() + node.bbox._max.y()) * 0.5f;
    float cz = (node.bbox._min.z() + node.bbox._max.z()) * 0.5f;
    float hx = (node.bbox._max.x() - node.bbox._min.x()) * 0.5f;
    float hy = (node.bbox._max.y() - node.bbox._min.y()) * 0.5f;
    float hz = (node.bbox._max.z() - node.bbox._min.z()) * 0.5f;
    jnode["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz};
  }

  jnode["geometricError"] = node.geometricError;

  if (node.isLeaf()) {
    // 叶节点：直接引用原始 Block 的 tileset.json
    if (node.blockIndices.size() == 1) {
      // 单 Block 叶节点
      jnode["content"]["uri"] =
          "../" + blocks[node.blockIndices[0]].blockTilesetRelPath;
      jnode["geometricError"] = blocks[node.blockIndices[0]].geometricError;
    } else {
      // 多 Block 叶节点（同时带 content 和 children）
      // content 是合并后的简化 b3dm
      // children 是各个 Block
      jnode["content"]["uri"] = "./" + node.name + ".b3dm";
      json children = json::array();
      for (int bi : node.blockIndices) {
        json child;
        const auto &blk = blocks[bi];
        if (blk.bbox.valid()) {
          float cx = (blk.bbox._min.x() + blk.bbox._max.x()) * 0.5f;
          float cy = (blk.bbox._min.y() + blk.bbox._max.y()) * 0.5f;
          float cz = (blk.bbox._min.z() + blk.bbox._max.z()) * 0.5f;
          float hx = (blk.bbox._max.x() - blk.bbox._min.x()) * 0.5f;
          float hy = (blk.bbox._max.y() - blk.bbox._min.y()) * 0.5f;
          float hz = (blk.bbox._max.z() - blk.bbox._min.z()) * 0.5f;
          child["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0,
                                            0,  hy, 0,  0,  0, hz};
        }
        child["geometricError"] = blk.geometricError;
        child["content"]["uri"] = "../" + blk.blockTilesetRelPath;
        children.push_back(child);
      }
      jnode["children"] = std::move(children);
    }
  } else {
    // 内部节点：同时有 content（简化 b3dm）和 children
    jnode["content"]["uri"] = "./" + node.name + ".b3dm";
    json children = json::array();
    for (const auto &child : node.children) {
      children.push_back(buildTopTilesetNode(child, blocks));
    }
    jnode["children"] = std::move(children);
  }

  return jnode;
}

bool TopLevelMerger::updateRootTileset(const std::string &outputDir,
                                       const osg::BoundingBox &globalBbox) {
  std::string tilesetPath = outputDir + "/tileset.json";

  // 备份原始 tileset.json
  if (fs::exists(tilesetPath)) {
    std::string backupPath = outputDir + "/_tileset.json";
    try {
      fs::copy_file(tilesetPath, backupPath,
                    fs::copy_options::overwrite_existing);
      LOG_INFO("Backed up original tileset.json -> _tileset.json");
    } catch (const std::exception &e) {
      LOG_WARN("Failed to backup tileset.json: " + std::string(e.what()));
    }
  }

  // 读取现有 tileset.json 以获取 transform 和 asset 信息
  json existingTs;
  {
    std::ifstream ifs(tilesetPath);
    if (ifs) {
      try {
        ifs >> existingTs;
      } catch (...) {
      }
    }
  }

  // 构建新的根 tileset.json
  json newTs;
  newTs["asset"] = existingTs.value(
      "asset", json({{"version", "1.0"}, {"gltfUpAxis", "Y"}}));
  newTs["geometricError"] = existingTs.value("geometricError", 2000);

  json root;
  // 保留原始 transform
  if (existingTs.contains("root") && existingTs["root"].contains("transform")) {
    root["transform"] = existingTs["root"]["transform"];
  }

  // 包围盒
  if (globalBbox.valid()) {
    float cx = (globalBbox._min.x() + globalBbox._max.x()) * 0.5f;
    float cy = (globalBbox._min.y() + globalBbox._max.y()) * 0.5f;
    float cz = (globalBbox._min.z() + globalBbox._max.z()) * 0.5f;
    float hx = (globalBbox._max.x() - globalBbox._min.x()) * 0.5f;
    float hy = (globalBbox._max.y() - globalBbox._min.y()) * 0.5f;
    float hz = (globalBbox._max.z() - globalBbox._min.z()) * 0.5f;
    root["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz};
  }

  root["geometricError"] = existingTs.value("geometricError", 2000);

  // 单子节点引用 top/tileset.json
  json topChild;
  topChild["geometricError"] = existingTs.value("geometricError", 2000);
  if (root.contains("boundingVolume"))
    topChild["boundingVolume"] = root["boundingVolume"];
  topChild["content"]["uri"] = "./top/tileset.json";
  root["children"] = json::array({topChild});

  newTs["root"] = root;

  // 写出
  std::ofstream ofs(tilesetPath);
  if (!ofs) {
    LOG_ERROR("Cannot write updated tileset.json");
    return false;
  }
  ofs << newTs.dump(2);
  LOG_INFO("Updated root tileset.json with top-level merge reference");
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// 主入口
// ════════════════════════════════════════════════════════════════════════════

bool TopLevelMerger::merge(const std::vector<MergeBlockInfo> &blocks,
                           const ConvertConfig &cfg,
                           const std::string &outputDir) {
  if (blocks.size() < 2) {
    LOG_INFO("TopMerge: only " + std::to_string(blocks.size()) +
             " block(s), skipping merge");
    return true;
  }

  LOG_INFO("=== Top-Level Root Node Merge ===");
  LOG_INFO("Total blocks: " + std::to_string(blocks.size()));

  // 1. 构建合并树
  MergeTreeNode tree = buildMergeTree(blocks);

  // 计算 geometricError 梯度
  // 根据样例：最顶层 ~56，每层约 /2
  double maxBlockError = 0;
  for (const auto &b : blocks)
    maxBlockError = std::max(maxBlockError, b.geometricError);

  // 计算树深度
  std::function<int(const MergeTreeNode &)> treeDepth;
  treeDepth = [&](const MergeTreeNode &n) -> int {
    if (n.isLeaf())
      return 0;
    int d = 0;
    for (const auto &c : n.children)
      d = std::max(d, treeDepth(c));
    return d + 1;
  };
  int depth = treeDepth(tree);

  // 设置 geometricError：从叶节点向上递增
  double baseError = maxBlockError * 2.0;
  std::function<void(MergeTreeNode &, int)> setErrors;
  setErrors = [&](MergeTreeNode &n, int lvl) {
    if (n.isLeaf()) {
      n.geometricError = baseError;
    } else {
      for (auto &c : n.children)
        setErrors(c, lvl + 1);
      // 父节点 error = max(children error) * 2
      double maxChildErr = 0;
      for (const auto &c : n.children)
        maxChildErr = std::max(maxChildErr, c.geometricError);
      n.geometricError = maxChildErr * 2.0;
    }
  };
  setErrors(tree, 0);

  // 2. 创建 top/ 目录
  std::string topDir = outputDir + "/top";
  fs::create_directories(topDir);

  // 3. 为每个节点生成合并瓦片（自底向上）
  std::string ext = (cfg.tileFormat == "b3dm") ? ".b3dm" : ".glb";
  std::function<bool(MergeTreeNode &)> generateTiles;
  generateTiles = [&](MergeTreeNode &node) -> bool {
    // 递归处理子节点
    for (auto &child : node.children) {
      if (!generateTiles(child))
        return false;
    }

    // 收集此节点关联的所有 Block 索引（包括子树中的所有）
    std::vector<int> allBlocks;
    std::function<void(const MergeTreeNode &)> collectBlocks;
    collectBlocks = [&](const MergeTreeNode &n) {
      for (int bi : n.blockIndices)
        allBlocks.push_back(bi);
      // 子节点的 block 不会重复，因为分裂时是互斥的
    };
    collectBlocks(node);

    // 去重
    std::set<int> unique(allBlocks.begin(), allBlocks.end());
    allBlocks.assign(unique.begin(), unique.end());

    if (allBlocks.empty())
      return true;

    // 单 Block 叶节点不需要生成合并瓦片
    if (node.isLeaf() && allBlocks.size() == 1)
      return true;

    // 简化比例：越靠近根，简化越激进
    float ratio = 0.1f;
    if (allBlocks.size() > 6)
      ratio = 0.05f;
    if (allBlocks.size() > 12)
      ratio = 0.03f;

    LOG_INFO("Generating merged tile: " + node.name + " (" +
             std::to_string(allBlocks.size()) +
             " blocks, ratio=" + std::to_string(ratio) + ")");

    MergedMesh mesh = mergeBlocks(blocks, allBlocks, ratio);
    if (mesh.vertices.empty()) {
      LOG_WARN("No geometry for merged node: " + node.name);
      return true; // 不是致命错误
    }

    std::string outPath = topDir + "/" + node.name + ext;
    if (!writeMergedTile(mesh, outPath, cfg)) {
      LOG_ERROR("Failed to write merged tile: " + outPath);
      return false;
    }

    LOG_INFO("  -> " + outPath + " (" +
             std::to_string(fs::file_size(outPath) / 1024) + " KB, " +
             std::to_string(mesh.indices.size() / 3) + " tris)");

    return true;
  };

  if (!generateTiles(tree)) {
    LOG_ERROR("Top-level merge failed during tile generation");
    return false;
  }

  // 4. 生成 top/tileset.json
  json topTs;
  topTs["asset"] = {
      {"version", "1.0"}, {"gltfUpAxis", "Y"}, {"generator", "osgb2tiles"}};
  topTs["geometricError"] = tree.geometricError * 2.0;

  json topRoot;
  topRoot["geometricError"] = tree.geometricError * 2.0;
  if (tree.bbox.valid()) {
    float cx = (tree.bbox._min.x() + tree.bbox._max.x()) * 0.5f;
    float cy = (tree.bbox._min.y() + tree.bbox._max.y()) * 0.5f;
    float cz = (tree.bbox._min.z() + tree.bbox._max.z()) * 0.5f;
    float hx = (tree.bbox._max.x() - tree.bbox._min.x()) * 0.5f;
    float hy = (tree.bbox._max.y() - tree.bbox._min.y()) * 0.5f;
    float hz = (tree.bbox._max.z() - tree.bbox._min.z()) * 0.5f;
    topRoot["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0,
                                        0,  hy, 0,  0,  0, hz};
  }

  // 构建树根 JSON 节点
  json treeContent = buildTopTilesetNode(tree, blocks);
  topRoot["children"] = json::array({treeContent});

  topTs["root"] = topRoot;

  std::string topTsPath = topDir + "/tileset.json";
  {
    std::ofstream ofs(topTsPath);
    if (!ofs) {
      LOG_ERROR("Cannot write top/tileset.json");
      return false;
    }
    ofs << topTs.dump(2);
  }
  LOG_INFO("Wrote top/tileset.json");

  // 5. 更新根 tileset.json
  osg::BoundingBox globalBbox;
  for (const auto &b : blocks) {
    if (b.bbox.valid()) {
      globalBbox.expandBy(b.bbox._min);
      globalBbox.expandBy(b.bbox._max);
    }
  }

  if (!updateRootTileset(outputDir, globalBbox)) {
    LOG_WARN(
        "Failed to update root tileset.json, top/ merge data still usable");
  }

  LOG_INFO("=== Top-Level Merge Complete ===");
  return true;
}
