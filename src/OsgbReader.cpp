#include "OsgbReader.h"
#include "Logger.h"

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/LOD>
#include <osg/Material>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <osgUtil/SmoothingVisitor>


#include <cassert>
#include <cstring>
#include <filesystem>
#include <stdexcept>


#include "WinUtils.h" // toShortPath()：解决 osgDB 不支持中文路径的问题

namespace fs = std::filesystem;

// ────────────────────────────────────────────────────────────────────────────
// 内部辅助 NodeVisitor：收集所有 Geode
// ────────────────────────────────────────────────────────────────────────────
class GeodeCollector : public osg::NodeVisitor {
public:
  std::vector<osg::Geode *> geodes;
  GeodeCollector() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
  void apply(osg::Geode &g) override { geodes.push_back(&g); }
};

// 从 StateSet 提取纹理（优先直接读内嵌像素，退回文件路径）
// 返回文件名（可能为空），同时尝试填充 out 的像素字段
template <typename T>
static std::string extractTexFromSS(osg::StateSet *ss, T *out = nullptr) {
  if (!ss)
    return {};
  auto *texAttr = ss->getTextureAttribute(0, osg::StateAttribute::TEXTURE);
  if (!texAttr)
    return {};
  auto *tex2d = dynamic_cast<osg::Texture2D *>(texAttr);
  if (!tex2d)
    return {};
  const osg::Image *img = tex2d->getImage();
  if (!img)
    return {};

  // 尝试直接读内嵌像素（ContextCapture OSGB 把纹理嵌在文件里）
  if (out && img->data() && img->s() > 0 && img->t() > 0 &&
      out->textureData.empty()) {
    int w = img->s(), h = img->t();
    size_t rowBytes = img->getRowSizeInBytes();
    GLenum pf = img->getPixelFormat();
    // 0x1907=GL_RGB, 0x1908=GL_RGBA
    int srcChans = (pf == 0x1908) ? 4 : 3;
    bool validFmt = (pf == 0x1907 || pf == 0x1908);
    if (validFmt && img->getDataType() == 0x1401 /* GL_UNSIGNED_BYTE */) {
      out->textureData.resize(w * h * 3);
      for (int row = 0; row < h; ++row) {
        // OSG 默认 BOTTOM_LEFT 原点，需要垂直翻转以匹配 glTF UV（Y向下）
        int srcRow = h - 1 - row;
        const uint8_t *src = img->data() + (size_t)srcRow * rowBytes;
        uint8_t *dst = out->textureData.data() + (size_t)row * w * 3;
        if (srcChans == 3) {
          memcpy(dst, src, w * 3);
        } else {
          for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
          }
        }
      }
      out->texWidth = w;
      out->texHeight = h;
    }
  }
  return img->getFileName(); // 内嵌时这只是一个标签，可能为空
}

// 兼容旧调用：只取路径
static std::string extractTexPathFromSS(osg::StateSet *ss) {
  return extractTexFromSS<TileNode>(ss, nullptr);
}

// ────────────────────────────────────────────────────────────────────────────
// OsgbReader
// ────────────────────────────────────────────────────────────────────────────
OsgbReader::OsgbReader(bool verbose) : verbose_(verbose) {}

std::vector<TileNode> OsgbReader::scan(const std::string &rootDir) {
  std::vector<TileNode> roots;

  if (!fs::exists(rootDir)) {
    LOG_ERROR("Input directory not found: " + rootDir);
    return roots;
  }

  LOG_INFO("Scanning OSGB directory: " + rootDir);

  // 递归扫描函数：查找所有 blockDir/blockName.osgb 根瓦片
  // 支持格式：
  //   rootDir/Tile_xxx/Tile_xxx.osgb          （一层）
  //   rootDir/Data/Tile_xxx/Tile_xxx.osgb     （ContextCapture 两层）
  std::function<void(const std::string &, int)> scanDir;
  scanDir = [&](const std::string &dir, int depth) {
    if (depth > 3)
      return; // 最多扫描 3 层

    try {
      for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) {
          // 直接放在当前目录的 .osgb 文件
          if (entry.path().extension() == ".osgb") {
            TileNode root;
            root.nodeId = entry.path().stem().string();
            root.osgbPath = entry.path().string();
            root.level = 0;
            roots.push_back(std::move(root));
            LOG_DEBUG("Found root tile: " + entry.path().string());
          }
          continue;
        }

        std::string blockDir = entry.path().string();
        std::string blockName = entry.path().filename().string();
        std::string rootOsgb = blockDir + "/" + blockName + ".osgb";

        if (fs::exists(rootOsgb)) {
          // 找到根瓦片：blockDir/blockName.osgb
          TileNode root;
          root.nodeId = blockName;
          root.osgbPath = rootOsgb;
          root.level = 0;
          roots.push_back(std::move(root));
          LOG_DEBUG("Found root tile: " + rootOsgb);
        } else {
          // 当前子目录没有同名 .osgb，继续递归（如 Data/ 目录）
          scanDir(blockDir, depth + 1);
        }
      }
    } catch (const std::exception &e) {
      LOG_WARN("Directory scan error: " + std::string(e.what()));
    }
  };

  scanDir(rootDir, 0);

  LOG_INFO("Found " + std::to_string(roots.size()) + " root tiles.");
  return roots;
}

bool OsgbReader::loadGeometry(TileNode &node) {
  if (!fs::exists(node.osgbPath)) {
    LOG_WARN("OSGB file not found: " + node.osgbPath);
    return false;
  }

  LOG_DEBUG("Loading: " + node.osgbPath);

  // 中文路径修复：仅当路径含非 ASCII 字符时才转为 8.3 短路径
  // ASCII 路径（即使含 +/- 等特殊字符）直接传给 osgDB，避免内部子引用解析失败
  auto needsShortPath = [](const std::string &p) {
    for (unsigned char c : p)
      if (c > 127)
        return true;
    return false;
  };
  std::string osgbPathForOSG = needsShortPath(node.osgbPath)
                                   ? toShortPath(node.osgbPath)
                                   : node.osgbPath;
  osg::ref_ptr<osg::Node> osgNode = osgDB::readNodeFile(osgbPathForOSG);
  if (!osgNode.valid()) {
    LOG_ERROR("Failed to load OSGB: " + node.osgbPath);
    return false;
  }

  // 获取包围盒
  osg::BoundingSphere bs = osgNode->getBound();
  node.bbox._min = osg::Vec3f(
      bs.center() - osg::Vec3f(bs.radius(), bs.radius(), bs.radius()));
  node.bbox._max = osg::Vec3f(
      bs.center() + osg::Vec3f(bs.radius(), bs.radius(), bs.radius()));

  // 遍历子节点，收集 PagedLOD 的子文件路径
  // 注意：baseDir 保持使用原始 UTF-8 路径（std::filesystem 支持），
  // 子文件路径拼接后再通过 toShortPath 转换传给 osgDB
  std::string baseDir = osgDB::getFilePath(node.osgbPath);
  traverseNode(osgNode.get(), node, node.level, baseDir);

  // 提取几何
  extractGeometry(osgNode.get(), node);

  // ── 纹理路径解析（相对路径 → 绝对路径）────────────────────────
  // OSG 存储的 img->getFileName() 通常是裸文件名（无目录前缀）
  // 需要相对 .osgb 文件所在目录进行解析
  if (!node.texturePath.empty()) {
    fs::path tp(node.texturePath);
    if (!tp.is_absolute() || !fs::exists(tp)) {
      fs::path osgbDir = fs::path(node.osgbPath).parent_path();
      // 尝试1：直接在 .osgb 目录下找同名文件
      fs::path cand1 = osgbDir / tp.filename();
      if (fs::exists(cand1)) {
        node.texturePath = cand1.string();
      } else {
        // 尝试2：在 .osgb 目录下找（含子目录部分的）
        fs::path cand2 = osgbDir / tp;
        if (fs::exists(cand2)) {
          node.texturePath = cand2.string();
        }
        // 尝试3：在父目录（block目录）下查找
        else {
          fs::path cand3 = osgbDir.parent_path() / tp.filename();
          if (fs::exists(cand3)) {
            node.texturePath = cand3.string();
          }
        }
      }
    }
  }

  // 若无法线，重建
  if (node.normals.empty() && !node.vertices.empty()) {
    LOG_DEBUG("Rebuilding normals for: " + node.nodeId);
    rebuildNormals(node);
  }

  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// 三阶段流水线 Stage 1：只做文件 I/O + OSG 解析（必须单线程）
// ────────────────────────────────────────────────────────────────────────────
osg::ref_ptr<osg::Node> OsgbReader::readFile(const std::string &osgbPath) {
  if (!fs::exists(osgbPath)) {
    LOG_WARN("OSGB file not found: " + osgbPath);
    return {};
  }
  auto needsShortPath = [](const std::string &p) {
    for (unsigned char c : p)
      if (c > 127)
        return true;
    return false;
  };
  std::string pathForOSG =
      needsShortPath(osgbPath) ? toShortPath(osgbPath) : osgbPath;
  osg::ref_ptr<osg::Node> osgNode = osgDB::readNodeFile(pathForOSG);
  if (!osgNode.valid()) {
    LOG_ERROR("Failed to load OSGB: " + osgbPath);
  }
  return osgNode;
}

// ────────────────────────────────────────────────────────────────────────────
// 三阶段流水线 Stage 2：从已加载的 osgNode 提取几何+纹理像素（无OSG锁，可并行）
// ────────────────────────────────────────────────────────────────────────────
bool OsgbReader::extractFromNode(osg::ref_ptr<osg::Node> osgNode,
                                 const std::string &osgbPath, TileNode &node) {
  if (!osgNode.valid())
    return false;

  // 包围盒
  osg::BoundingSphere bs = osgNode->getBound();
  node.bbox._min = osg::Vec3f(
      bs.center() - osg::Vec3f(bs.radius(), bs.radius(), bs.radius()));
  node.bbox._max = osg::Vec3f(
      bs.center() + osg::Vec3f(bs.radius(), bs.radius(), bs.radius()));

  // PagedLOD 子路径（用原始 UTF-8 baseDir）
  std::string baseDir = osgDB::getFilePath(osgbPath);
  traverseNode(osgNode.get(), node, node.level, baseDir);

  // 几何 + 内嵌纹理像素提取（CPU密集，可并行）
  extractGeometry(osgNode.get(), node);

  // 纹理路径解析（相对路径 → 绝对路径）
  if (!node.texturePath.empty()) {
    fs::path tp(node.texturePath);
    if (!tp.is_absolute() || !fs::exists(tp)) {
      fs::path osgbDir = fs::path(osgbPath).parent_path();
      fs::path cand1 = osgbDir / tp.filename();
      if (fs::exists(cand1)) {
        node.texturePath = cand1.string();
      } else {
        fs::path cand2 = osgbDir / tp;
        if (fs::exists(cand2)) {
          node.texturePath = cand2.string();
        } else {
          fs::path cand3 = osgbDir.parent_path() / tp.filename();
          if (fs::exists(cand3))
            node.texturePath = cand3.string();
        }
      }
    }
  }

  // 法线重建
  if (node.normals.empty() && !node.vertices.empty()) {
    rebuildNormals(node);
  }

  return true;
}

void OsgbReader::traverseNode(osg::Node *osgnNode, TileNode &out, int level,
                              const std::string &baseDir) {
  if (!osgnNode)
    return;

  // PagedLOD：包含指向子级 .osgb 文件的引用
  if (auto *plod = dynamic_cast<osg::PagedLOD *>(osgnNode)) {
    for (unsigned i = 0; i < plod->getNumFileNames(); ++i) {
      std::string childFile = plod->getFileName(i);
      if (childFile.empty())
        continue;

      // 构建完整路径
      std::string fullPath = osgDB::concatPaths(baseDir, childFile);
      if (!fs::exists(fullPath)) {
        fullPath = childFile; // 尝试绝对路径
      }

      if (fs::exists(fullPath) &&
          osgDB::getLowerCaseFileExtension(fullPath) == "osgb") {
        out.children.push_back(fullPath);
        LOG_DEBUG("  Child: " + fullPath);
      }
    }
    // geometricError 估算：由 PagedLOD 的 range 推导
    // OSG API: LOD::getRangeList() 返回 vector<pair<float,float>>
    const osg::LOD::RangeList &ranges = plod->getRangeList();
    if (!ranges.empty()) {
      out.geometricError = static_cast<double>(ranges[0].second) * 0.002;
    }
  }

  // 普通 Group：继续向下遍历
  if (auto *grp = dynamic_cast<osg::Group *>(osgnNode)) {
    for (unsigned i = 0; i < grp->getNumChildren(); ++i) {
      traverseNode(grp->getChild(i), out, level + 1, baseDir);
    }
  }
}

void OsgbReader::extractGeometry(osg::Node *root, TileNode &out) {
  // 收集所有 Geode
  GeodeCollector collector;
  root->accept(collector);

  // ── 关键修复：每个 Geode 生成独立 SubMesh ──────────────────────
  // 中间层 OSGB 包含多个 Geode，每个 Geode 有独立纹理
  // 必须分开处理，否则所有 Geode 的几何会共用第一个纹理 → 纹理混乱
  for (auto *geode : collector.geodes) {
    for (unsigned d = 0; d < geode->getNumDrawables(); ++d) {
      auto *geom = dynamic_cast<osg::Geometry *>(geode->getDrawable(d));
      if (!geom)
        continue;

      const auto *va =
          dynamic_cast<const osg::Vec3Array *>(geom->getVertexArray());
      if (!va || va->empty())
        continue;

      // ── 创建新 SubMesh ──────────────────────────────────────
      SubMesh sm;

      // 纹理（优先内嵌像素，其次文件路径）
      // 注意：这里传 &sm 而非 &out，使每个 SubMesh 提取自己的纹理
      extractTexFromSS(geom->getStateSet(), &sm);
      if (sm.textureData.empty())
        extractTexFromSS(geode->getStateSet(), &sm);
      if (sm.texturePath.empty() && sm.textureData.empty()) {
        std::string p = extractTexPathFromSS(geom->getStateSet());
        if (p.empty())
          p = extractTexPathFromSS(geode->getStateSet());
        sm.texturePath = p;
      }

      // ── 顶点 ────────────────────────────────────────────────
      for (const auto &v : *va) {
        sm.vertices.push_back(v.x());
        sm.vertices.push_back(v.y());
        sm.vertices.push_back(v.z());
      }

      // ── 法线 ────────────────────────────────────────────────
      const auto *na =
          dynamic_cast<const osg::Vec3Array *>(geom->getNormalArray());
      if (na && na->size() == va->size()) {
        for (const auto &n : *na) {
          sm.normals.push_back(n.x());
          sm.normals.push_back(n.y());
          sm.normals.push_back(n.z());
        }
      } else {
        sm.normals.resize(va->size() * 3, 0.0f);
      }

      // ── UV（V 轴翻转）───────────────────────────────────────
      const auto *ta =
          dynamic_cast<const osg::Vec2Array *>(geom->getTexCoordArray(0));
      if (ta && ta->size() == va->size()) {
        for (const auto &uv : *ta) {
          sm.uvs.push_back(uv.x());
          sm.uvs.push_back(1.0f - uv.y()); // glTF V=0 顶部
        }
      } else {
        sm.uvs.resize(va->size() * 2, 0.0f);
      }

      // ── 索引 ────────────────────────────────────────────────
      for (unsigned p = 0; p < geom->getNumPrimitiveSets(); ++p) {
        const osg::PrimitiveSet *ps = geom->getPrimitiveSet(p);

        if (const auto *de32 =
                dynamic_cast<const osg::DrawElementsUInt *>(ps)) {
          for (size_t i = 0; i + 2 < de32->size(); i += 3) {
            sm.indices.push_back((*de32)[i]);
            sm.indices.push_back((*de32)[i + 1]);
            sm.indices.push_back((*de32)[i + 2]);
          }
        } else if (const auto *de16 =
                       dynamic_cast<const osg::DrawElementsUShort *>(ps)) {
          for (size_t i = 0; i + 2 < de16->size(); i += 3) {
            sm.indices.push_back((*de16)[i]);
            sm.indices.push_back((*de16)[i + 1]);
            sm.indices.push_back((*de16)[i + 2]);
          }
        } else if (const auto *da = dynamic_cast<const osg::DrawArrays *>(ps)) {
          if (da->getMode() == osg::PrimitiveSet::TRIANGLES) {
            uint32_t first = static_cast<uint32_t>(da->getFirst());
            for (uint32_t i = 0; i + 2 < static_cast<uint32_t>(da->getCount());
                 i += 3) {
              sm.indices.push_back(first + i);
              sm.indices.push_back(first + i + 1);
              sm.indices.push_back(first + i + 2);
            }
          }
        }
      }

      if (sm.vertices.empty() || sm.indices.empty())
        continue;

      // 加入 subMeshes 列表
      out.subMeshes.push_back(std::move(sm));
    }
  }

  // ── 同时合并到 out.vertices/etc. 用于 bbox 等兼容逻辑 ──────────
  for (const auto &sm : out.subMeshes) {
    uint32_t base = static_cast<uint32_t>(out.vertices.size() / 3);
    out.vertices.insert(out.vertices.end(), sm.vertices.begin(),
                        sm.vertices.end());
    out.normals.insert(out.normals.end(), sm.normals.begin(), sm.normals.end());
    out.uvs.insert(out.uvs.end(), sm.uvs.begin(), sm.uvs.end());
    for (uint32_t idx : sm.indices)
      out.indices.push_back(base + idx);
  }
  // 第一个 SubMesh 的纹理设为 out 的主纹理（兼容旧路径）
  if (!out.subMeshes.empty()) {
    out.textureData = out.subMeshes[0].textureData;
    out.texWidth = out.subMeshes[0].texWidth;
    out.texHeight = out.subMeshes[0].texHeight;
    out.texturePath = out.subMeshes[0].texturePath;
  }

  LOG_DEBUG("  Extracted: " + std::to_string(out.subMeshes.size()) +
            " subMeshes, " + std::to_string(out.vertices.size() / 3) +
            " verts, " + std::to_string(out.indices.size() / 3) + " tris");
}

// 从 StateSet 提取纹理路径的成员函数（由 extractTexPathFromSS 实现）
std::string OsgbReader::extractTexturePath(osg::Node *node) {
  auto *geode = dynamic_cast<osg::Geode *>(node);
  if (!geode)
    return {};
  // 先查 Geode 的 StateSet
  std::string p = extractTexPathFromSS(geode->getStateSet());
  if (!p.empty())
    return p;
  // 再查每个 Drawable 的 StateSet
  for (unsigned d = 0; d < geode->getNumDrawables(); ++d) {
    p = extractTexPathFromSS(geode->getDrawable(d)->getStateSet());
    if (!p.empty())
      return p;
  }
  return {};
}

void OsgbReader::rebuildNormals(TileNode &node) {
  const size_t triCount = node.indices.size() / 3;
  const size_t vtxCount = node.vertices.size() / 3;

  // 重置法线
  node.normals.assign(vtxCount * 3, 0.0f);

  // 逐三角形计算面法线并累加到顶点
  for (size_t t = 0; t < triCount; ++t) {
    uint32_t i0 = node.indices[t * 3], i1 = node.indices[t * 3 + 1],
             i2 = node.indices[t * 3 + 2];

    float *v0 = node.vertices.data() + i0 * 3;
    float *v1 = node.vertices.data() + i1 * 3;
    float *v2 = node.vertices.data() + i2 * 3;

    // 边向量
    float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
    float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];

    // 叉积
    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;

    for (uint32_t idx : {i0, i1, i2}) {
      node.normals[idx * 3] += nx;
      node.normals[idx * 3 + 1] += ny;
      node.normals[idx * 3 + 2] += nz;
    }
  }

  // 归一化
  for (size_t i = 0; i < vtxCount; ++i) {
    float *n = node.normals.data() + i * 3;
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-6f) {
      n[0] /= len;
      n[1] /= len;
      n[2] /= len;
    }
  }
}
