/**
 * osgb2tiles - OSGB 转 3D Tiles 命令行工具 v2.0
 *
 * 正确架构：
 *   1. 扫描找所有 Block 根目录 (Tile_+xxx_+yyy/)
 *   2. 对每个 Block 递归读取 PagedLOD 树结构（全部子节点 .osgb 文件）
 *   3. 对树中每个节点独立转换成同名 .b3dm，输出到 Data/Tile_xxx/ 子目录
 *   4. 根据 PagedLOD 引用关系生成每个 Block 的 tileset.json
 *   5. 全局 tileset.json 用 externalTileset 引用各 Block
 */

#include "Config.h"
#include "GeometryConverter.h"
#include "GlbWriter.h"
#include "Logger.h"
#include "MathUtils.h"
#include "MeshSimplifier.h"
#include "OsgbMetaReader.h"
#include "OsgbReader.h"
#include "TextureProcessor.h"
#include "ThreadPool.h"
#include "TilesetBuilder.h"
#include "TopLevelMerger.h"
#include "WinUtils.h" // toShortPath()：解决 osgDB 不支持中文路径的问题

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Node>
#include <osg/PagedLOD>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// ────────────────────────────────────────────────────────────────────────────
// 配置加载（从 JSON）
// ────────────────────────────────────────────────────────────────────────────
static ConvertConfig loadJsonConfig(const std::string &path) {
  ConvertConfig cfg;
  std::ifstream f(path);
  if (!f)
    return cfg;
  json j = json::parse(f, nullptr, false);
  if (j.is_discarded())
    return cfg;

  auto get = [&](auto &field, const char *key) {
    if (j.contains(key) && !j[key].is_null())
      field = j[key].get<std::remove_reference_t<decltype(field)>>();
  };
  // ── 路径 ──────────────────────────────────────
  get(cfg.inputPath, "inputPath");
  get(cfg.outputPath, "outputPath");
  // ── 地理坐标 ───────────────────────────────────
  get(cfg.longitude, "longitude");
  get(cfg.latitude, "latitude");
  get(cfg.height, "height");
  // ── 网格 ───────────────────────────────────────
  get(cfg.simplifyMesh, "simplifyMesh");
  get(cfg.simplifyRatio, "simplifyRatio");
  get(cfg.simplifyError, "simplifyError");
  get(cfg.mergeLevel, "mergeLevel");
  get(cfg.mergeRoot, "mergeRoot");
  // ── 纹理 ───────────────────────────────────────
  get(cfg.compressTexture, "compressTexture");
  get(cfg.textureFormat, "textureFormat");
  get(cfg.maxTextureSize, "maxTextureSize");
  get(cfg.jpegQuality, "jpegQuality");
  get(cfg.webpQuality, "webpQuality");
  get(cfg.webpLossless, "webpLossless");
  // ── 几何压缩 ───────────────────────────────────
  get(cfg.compressGeometry, "compressGeometry");
  get(cfg.dracoQuantBits, "dracoQuantBits");
  get(cfg.geometricErrorScale, "geometricErrorScale");
  // ktx2 texture compression
  get(cfg.ktx2Mode, "ktx2Mode");
  get(cfg.ktx2Quality, "ktx2Quality");
  get(cfg.ktx2Mipmaps, "ktx2Mipmaps");
  // ── 输出格式 ───────────────────────────────────
  get(cfg.tileFormat, "tileFormat");
  get(cfg.refineMode, "refineMode");
  get(cfg.writeGzip, "writeGzip");
  // ── 性能 ───────────────────────────────────────
  get(cfg.threads, "threads");
  // ── 调试 ───────────────────────────────────────
  get(cfg.verbose, "verbose");

  // ── 七参数 Bursa-Wolf（嵌套 JSON 对象） ─────────
  if (j.contains("transform7p") && j["transform7p"].is_object()) {
    const auto &t = j["transform7p"];
    auto getT = [&](double &field, const char *key) {
      if (t.contains(key) && !t[key].is_null())
        field = t[key].get<double>();
    };
    auto getB = [&](bool &field, const char *key) {
      if (t.contains(key) && !t[key].is_null())
        field = t[key].get<bool>();
    };
    getB(cfg.transform7p.enabled, "enabled");
    getT(cfg.transform7p.dx, "dx");
    getT(cfg.transform7p.dy, "dy");
    getT(cfg.transform7p.dz, "dz");
    getT(cfg.transform7p.rx, "rx");
    getT(cfg.transform7p.ry, "ry");
    getT(cfg.transform7p.rz, "rz");
    getT(cfg.transform7p.scale, "scale");
  }
  return cfg;
}

// ────────────────────────────────────────────────────────────────────────────
// OsgbNode：描述 PagedLOD 树中的一个节点
// ────────────────────────────────────────────────────────────────────────────
struct OsgbNode {
  std::string osgbPath;           // 本节点 .osgb 绝对路径
  std::string nodeId;             // 文件名（不含扩展名），用于 b3dm 命名
  osg::BoundingBox bbox;          // 局部包围盒
  double geometricError;          // 来自 LOD/PagedLOD range
  std::vector<OsgbNode> children; // 子节点
};

// ────────────────────────────────────────────────────────────────────────────
// 从 OSG 节点提取 PagedLOD 的子文件引用 + geometricError
// ────────────────────────────────────────────────────────────────────────────
static void collectPagedLODChildren(osg::Node *root, const std::string &baseDir,
                                    std::vector<std::string> &childPaths,
                                    double &geoError) {
  if (!root)
    return;

  // PagedLOD（最常见的倾斜摄影结构）
  if (auto *plod = dynamic_cast<osg::PagedLOD *>(root)) {
    // geometricError 由切换距离推导（最高 range 上限 × 像素角近似）
    auto &ranges = plod->getRangeList();
    if (!ranges.empty()) {
      geoError = static_cast<double>(ranges[0].second) * 0.002;
    }
    for (unsigned i = 0; i < plod->getNumFileNames(); ++i) {
      std::string fn = plod->getFileName(i);
      if (fn.empty())
        continue;
      std::string full = osgDB::concatPaths(baseDir, fn);
      if (!fs::exists(full))
        full = fn;
      if (fs::exists(full) &&
          osgDB::getLowerCaseFileExtension(full) == "osgb") {
        childPaths.push_back(full);
      }
    }
  }

  // LOD（非 Paged 版本）
  if (auto *lod = dynamic_cast<osg::LOD *>(root)) {
    if (lod->getNumRanges() > 0) {
      geoError = static_cast<double>(lod->getMaxRange(0)) * 0.002;
    }
  }

  // 递归遍历子节点（非 PagedLOD 子节点，即静态几何子图）
  osg::Group *grp = root->asGroup();
  if (grp) {
    for (unsigned i = 0; i < grp->getNumChildren(); ++i) {
      collectPagedLODChildren(grp->getChild(i), baseDir, childPaths, geoError);
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// 递归构建 OsgbNode 树（只读 .osgb 文件头，获取 bbox 和子引用）
// ────────────────────────────────────────────────────────────────────────────
static OsgbNode buildOsgbTree(const std::string &osgbPath, int maxDepth = 30) {
  OsgbNode node;
  node.osgbPath = osgbPath;
  node.nodeId = fs::path(osgbPath).stem().string();
  node.geometricError = 0.0;

  if (!fs::exists(osgbPath))
    return node;

  // 中文路径修复：仅当路径含非 ASCII 字符（中文等）时才转为 8.3 短路径
  // 注意：对 ASCII 路径（即使含 + 等特殊字符）勿转短路径，否则 osgDB
  // 内部子引用解析会失败
  auto needsShortPath = [](const std::string &p) {
    for (unsigned char c : p)
      if (c > 127)
        return true;
    return false;
  };
  std::string osgbPathForOSG =
      needsShortPath(osgbPath) ? toShortPath(osgbPath) : osgbPath;
  osg::ref_ptr<osg::Node> osgNode = osgDB::readNodeFile(osgbPathForOSG);
  if (!osgNode.valid())
    return node;

  // BoundingBox
  osg::BoundingSphere bs = osgNode->getBound();
  if (bs.valid()) {
    float r = bs.radius();
    node.bbox._min = osg::Vec3f(bs.center() - osg::Vec3f(r, r, r));
    node.bbox._max = osg::Vec3f(bs.center() + osg::Vec3f(r, r, r));
  }

  // 收集子引用
  std::string baseDir = osgDB::getFilePath(osgbPath);
  std::vector<std::string> childPaths;
  collectPagedLODChildren(osgNode.get(), baseDir, childPaths,
                          node.geometricError);

  // 递归构建子树
  if (maxDepth > 0) {
    for (const auto &cp : childPaths) {
      node.children.push_back(buildOsgbTree(cp, maxDepth - 1));
    }
  }

  return node;
}

// ────────────────────────────────────────────────────────────────────────────
// 单个节点转换：.osgb → .b3dm / .glb
// ────────────────────────────────────────────────────────────────────────────
static bool convertOsgbFile(const std::string &osgbPath,
                            const std::string &outFile,
                            const ConvertConfig &cfg) {
  if (!fs::exists(osgbPath))
    return false;

  TileNode node;
  node.nodeId = fs::path(osgbPath).stem().string();
  node.osgbPath = osgbPath;
  node.level = 0;

  OsgbReader reader(cfg.verbose);
  if (!reader.loadGeometry(node))
    return false;

  MeshSimplifier simplifier(cfg);
  simplifier.simplify(node);

  GeometryConverter converter(cfg);
  converter.transform(node);

  TextureProcessor texProc(cfg);
  texProc.process(node);
  auto texData = texProc.encode(node);
  auto mimeType = texProc.mimeType();

  GlbWriter writer(cfg);
  return writer.write(node, texData, mimeType, outFile);
}

// ────────────────────────────────────────────────────────────────────────────
// 生产者-消费者流水线：有界并发队列 + 纯CPU处理阶段（无OSG全局锁）
// ────────────────────────────────────────────────────────────────────────────

/// 有界阻塞队列（超过容量自动背压，setDone后pop返回false）
template <typename T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t cap) : cap_(cap) {}

  void push(T item) {
    std::unique_lock<std::mutex> lk(mtx_);
    cvPush_.wait(lk, [&] { return q_.size() < cap_ || done_; });
    if (!done_) {
      q_.push(std::move(item));
      cvPop_.notify_one();
    }
  }

  bool pop(T &item) {
    std::unique_lock<std::mutex> lk(mtx_);
    cvPop_.wait(lk, [&] { return !q_.empty() || done_; });
    if (q_.empty())
      return false;
    item = std::move(q_.front());
    q_.pop();
    cvPush_.notify_one();
    return true;
  }

  void setDone() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      done_ = true;
    }
    cvPop_.notify_all();
    cvPush_.notify_all();
  }

private:
  std::queue<T> q_;
  std::mutex mtx_;
  std::condition_variable cvPush_, cvPop_;
  size_t cap_;
  bool done_ = false;
};

/// Pipeline 工作单元：已加载的 TileNode + 目标输出路径
struct WorkItem {
  TileNode node;
  std::string outFile;
};

/// 纯CPU处理阶段（不调用osgDB，无全局锁）
/// 由 Worker 线程并行执行：simplify → transform → encode texture → write GLB
static bool processAlreadyLoaded(TileNode &node, const std::string &outFile,
                                 const ConvertConfig &cfg) {
  MeshSimplifier simplifier(cfg);
  simplifier.simplify(node);

  GeometryConverter converter(cfg);
  converter.transform(node);

  TextureProcessor texProc(cfg);
  texProc.process(node);
  auto texData = texProc.encode(node);
  auto mimeType = texProc.mimeType();

  GlbWriter writer(cfg);
  return writer.write(node, texData, mimeType, outFile);
}

// ────────────────────────────────────────────────────────────────────────────
// 递归生成 Block 的 tileset.json 节点（Pattern 1：包围盒用本地 ENU box 格式）
// depth=0 为 Block 根节点，geometricError 按 16/2^depth 递减
// ────────────────────────────────────────────────────────────────────────────
// 计算包围盒对角线长度辅助函数
static double bboxDiag(const osg::BoundingBox &bb) {
  if (!bb.valid())
    return -1.0;
  osg::Vec3f e = bb._max - bb._min;
  return std::sqrt(double(e.x() * e.x()) + double(e.y() * e.y()) +
                   double(e.z() * e.z()));
}

static json buildTilesetNode(const OsgbNode &node, const ConvertConfig &cfg,
                             int depth = 0) {

  json tile;

  // Pattern 1: 顶点在本地 ENU 坐标系，包围盒用 box 格式（单位：米）
  if (node.bbox.valid()) {
    float cx = (node.bbox._min.x() + node.bbox._max.x()) * 0.5f;
    float cy = (node.bbox._min.y() + node.bbox._max.y()) * 0.5f;
    float cz = (node.bbox._min.z() + node.bbox._max.z()) * 0.5f;
    float hx = (node.bbox._max.x() - node.bbox._min.x()) * 0.5f;
    float hy = (node.bbox._max.y() - node.bbox._min.y()) * 0.5f;
    float hz = (node.bbox._max.z() - node.bbox._min.z()) * 0.5f;
    if (std::isfinite(cx) && hx > 0.0f) {
      tile["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0,
                                       0,  hy, 0,  0,  0, hz};
    }
  }

  // 若包围盒无效，用安全默认値
  if (!tile.contains("boundingVolume")) {
    tile["boundingVolume"]["box"] = {0.0, 0.0,   100.0, 500.0, 0, 0,
                                     0,   500.0, 0,     0,     0, 200.0};
  }

  // ── geometricError: 基于包围盒对角线 * scale （P2 修正） ─────────────────
  // 叶子节点=0；其他节点根据包围盒大小动态计算，确保父节点误差始终大于子节点
  double geoErr = 0.0;
  if (!node.children.empty()) {
    double d = bboxDiag(node.bbox);
    if (d > 0.0) {
      geoErr = d * cfg.geometricErrorScale;
    } else if (node.geometricError > 0.0) {
      // PagedLOD range 回退：直接用它（已为米单位）
      geoErr = node.geometricError * cfg.geometricErrorScale;
    } else {
      // 深度回退，和原来行为一致
      geoErr = 16.0 / std::pow(2.0, depth);
    }
  }
  tile["geometricError"] = geoErr;
  tile["refine"] = cfg.refineMode;

  // content URI
  std::string ext = (cfg.tileFormat == "b3dm") ? ".b3dm" : ".glb";
  tile["content"]["uri"] = "./" + node.nodeId + ext;

  // children
  if (!node.children.empty()) {
    json children = json::array();
    for (const auto &child : node.children) {
      children.push_back(buildTilesetNode(child, cfg, depth + 1));
    }
    tile["children"] = std::move(children);
  }

  return tile;
}

// ────────────────────────────────────────────────────────────────────────────
// 处理一个 Block（Tile_xxx_yyy）：
//   1. 构建 PagedLOD 树
//   2. 多线程转换树中所有节点
//   3. 写 Block 的 tileset.json
// ────────────────────────────────────────────────────────────────────────────
struct BlockResult {
  std::string blockName;
  std::string blockTilesetRelPath; // 相对全局输出目录的路径
  osg::BoundingBox bbox;
  bool ok = false;
};

static BlockResult processBlock(const std::string &rootOsgbPath,
                                const std::string &globalOutputDir,
                                const ConvertConfig &cfg, ThreadPool &pool,
                                std::atomic<int> &totalConverted,
                                std::mutex &logMtx) {
  BlockResult result;
  result.blockName = fs::path(rootOsgbPath).stem().string();

  // Output sub-dir: globalOutputDir/Data/Tile_xxx/
  std::string blockName = result.blockName;
  // If root .osgb is named "Tile_+007_+009.osgb", the containing folder is
  // Tile_+007_+009 We use the parent directory name as block name
  std::string blockDirName =
      fs::path(rootOsgbPath).parent_path().filename().string();
  std::string blockOutDir = globalOutputDir + "/Data/" + blockDirName;
  fs::create_directories(blockOutDir);

  result.blockTilesetRelPath = "Data/" + blockDirName + "/tileset.json";

  // 1. 构建 PagedLOD 树
  LOG_DEBUG("Building PagedLOD tree for: " + blockName);
  OsgbNode tree = buildOsgbTree(rootOsgbPath);
  result.bbox = tree.bbox;

  // 2. 收集树中所有节点的 osgbPath（DFS）
  std::vector<std::string> allOsgbPaths;
  std::function<void(const OsgbNode &)> collectAllPaths;
  collectAllPaths = [&](const OsgbNode &n) {
    allOsgbPaths.push_back(n.osgbPath);
    for (const auto &c : n.children)
      collectAllPaths(c);
  };
  collectAllPaths(tree);

  LOG_DEBUG("  Found " + std::to_string(allOsgbPaths.size()) +
            " nodes in tree");

  // 3. Three-stage pipeline:
  //   Stage1: 1 std::thread  - osgDB::readNodeFile (OSG global lock, serial)
  //   Stage2: N std::threads - extractFromNode, parallel JPEG decode (no pool,
  //   avoids deadlock) Stage3: N pool threads - simplify/encode/write (uses
  //   pool)
  size_t numWorkers = std::max<size_t>(1, pool.size());
  std::string ext = (cfg.tileFormat == "b3dm") ? ".b3dm" : ".glb";

  struct NodeItem {
    osg::ref_ptr<osg::Node> osgNode;
    std::string osgbPath;
    std::string outFile;
  };
  BoundedQueue<NodeItem> nodeQueue(numWorkers * 2);
  BoundedQueue<WorkItem> workQueue(numWorkers * 3);

  // Stage1: Reader single thread, only osgDB::readNodeFile
  std::thread readerThread([&]() {
    OsgbReader reader1(cfg.verbose);
    for (const auto &osgbPath : allOsgbPaths) {
      if (!fs::exists(osgbPath))
        continue;
      auto osgNode = reader1.readFile(osgbPath);
      if (osgNode.valid()) {
        std::string stem = fs::path(osgbPath).stem().string();
        nodeQueue.push(NodeItem{std::move(osgNode), osgbPath,
                                blockOutDir + "/" + stem + ext});
      }
    }
    nodeQueue.setDone();
  });

  // Stage2: N independent std::threads for geometry+JPEG decode
  // NOTE: Must NOT use pool.submit() here - would deadlock if pool is full with
  // Stage2 tasks
  //       while Stage3 tasks are waiting in pool queue and workQueue is full.
  std::vector<std::thread> extractThreads;
  extractThreads.reserve(numWorkers);
  for (size_t wi = 0; wi < numWorkers; ++wi) {
    extractThreads.emplace_back([&]() {
      OsgbReader extractor(cfg.verbose);
      NodeItem nitem;
      while (nodeQueue.pop(nitem)) {
        WorkItem witem;
        witem.node.nodeId = fs::path(nitem.osgbPath).stem().string();
        witem.node.osgbPath = nitem.osgbPath;
        witem.node.level = 0;
        witem.outFile = nitem.outFile;
        if (extractor.extractFromNode(nitem.osgNode, nitem.osgbPath,
                                      witem.node)) {
          workQueue.push(std::move(witem));
        }
      }
    });
  }

  // Stage3: N pool workers, parallel simplify+encode+write
  std::atomic<int> ok{0}, fail{0};
  std::vector<std::future<void>> workerFuts;
  for (size_t wi = 0; wi < numWorkers; ++wi) {
    workerFuts.emplace_back(pool.submit([&]() {
      WorkItem item;
      while (workQueue.pop(item)) {
        if (processAlreadyLoaded(item.node, item.outFile, cfg))
          ++ok;
        else
          ++fail;
      }
    }));
  }

  // Wait order: Reader -> Stage2 threads -> close workQueue -> Stage3
  readerThread.join();
  for (auto &t : extractThreads)
    t.join();
  workQueue.setDone();
  for (auto &f : workerFuts)
    f.get();

  totalConverted += ok;

  {
    std::lock_guard<std::mutex> lk(logMtx);
    LOG_INFO("Block " + blockDirName + ": " + std::to_string(ok) + " OK, " +
             std::to_string(fail) + " fail");
  }

  // 4. 写 Block 的 tileset.json
  json blockTileset;
  blockTileset["asset"]["version"] = "1.0";
  blockTileset["asset"]["gltfUpAxis"] = "Z";

  // 根节点 geometricError 固定为 1000.0（与参考输出一致）
  // 块根节点 geometricError：基于包围盒对角线（P2）
  double blockDiag = bboxDiag(tree.bbox);
  double ROOT_GEO_ERROR =
      (blockDiag > 0.0) ? blockDiag * cfg.geometricErrorScale * 2.0 : 1000.0;
  blockTileset["geometricError"] = ROOT_GEO_ERROR;

  json rootTile = buildTilesetNode(tree, cfg);
  rootTile["geometricError"] = ROOT_GEO_ERROR;
  blockTileset["root"] = rootTile;

  std::string tilesetPath = blockOutDir + "/tileset.json";
  std::ofstream ofs(tilesetPath);
  if (ofs) {
    ofs << blockTileset.dump(2);
    LOG_DEBUG("  Wrote " + tilesetPath);
    result.ok = true;
  }

  return result;
}

// ────────────────────────────────────────────────────────────────────────────
// 根节点合并辅助函数
// ────────────────────────────────────────────────────────────────────────────

/// 自动计算合并层级：使最终叶节点平均块数 <= targetLeafCount
/// 目标16个叶节点（4x4象限），确保130块数据分出3层四叉树
static int autoComputeMergeLevel(int totalBlocks, int targetLeafCount = 16) {
  if (totalBlocks <= targetLeafCount)
    return 0;
  double ratio = static_cast<double>(totalBlocks) / targetLeafCount;
  int level = static_cast<int>(std::ceil(std::log(ratio) / std::log(4.0)));
  return std::min(level, 6); // 最大6层（4^6=4096个叶节点）
}

/// 解析 Block 名称中的网格坐标，例如 "Tile_+007_+009" → (7, 9)
/// 失败时返回 (0, 0)
static std::pair<int, int> parseBlockCoord(const std::string &blockName) {
  // 格式: Tile_+XXX_+YYY 或 Tile_-XXX_-YYY
  int x = 0, y = 0;
  // 查找两个数字段（含符号）
  std::vector<int> nums;
  size_t i = 0;
  while (i < blockName.size() && nums.size() < 2) {
    if (blockName[i] == '+' || blockName[i] == '-' ||
        (blockName[i] >= '0' && blockName[i] <= '9')) {
      // 确认是数字起始（前面是 _ 分隔符）
      if (i > 0 && blockName[i - 1] == '_') {
        char *end;
        long val = std::strtol(blockName.c_str() + i, &end, 10);
        if (end != blockName.c_str() + i) {
          nums.push_back(static_cast<int>(val));
          i = end - blockName.c_str();
          continue;
        }
      }
    }
    ++i;
  }
  if (nums.size() >= 2) {
    x = nums[0];
    y = nums[1];
  }
  return {x, y};
}

/// 计算 BlockResult 列表的合并包围盒
static osg::BoundingBox
mergeBlockBboxes(const std::vector<const BlockResult *> &blocks) {
  osg::BoundingBox merged;
  for (const auto *br : blocks) {
    if (br->ok && br->bbox.valid()) {
      merged.expandBy(br->bbox._min);
      merged.expandBy(br->bbox._max);
    }
  }
  return merged;
}

/// 递归构建四叉树节点（每层按 x/y 中点切分为4个象限）
/// level=0 是当前层，maxLevel 是最大合并层数
static json buildQuadTreeNode(const std::vector<const BlockResult *> &blocks,
                              int level, int maxLevel,
                              const ConvertConfig &cfg) {
  json node;

  // 计算包围盒
  osg::BoundingBox bbox = mergeBlockBboxes(blocks);
  if (bbox.valid()) {
    float cx = (bbox._min.x() + bbox._max.x()) * 0.5f;
    float cy = (bbox._min.y() + bbox._max.y()) * 0.5f;
    float cz = (bbox._min.z() + bbox._max.z()) * 0.5f;
    float hx = (bbox._max.x() - bbox._min.x()) * 0.5f + 10.0f;
    float hy = (bbox._max.y() - bbox._min.y()) * 0.5f + 10.0f;
    float hz = (bbox._max.z() - bbox._min.z()) * 0.5f + 20.0f;
    node["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz};
  } else {
    node["boundingVolume"]["box"] = {0.0, 0.0,    200.0, 5000.0, 0, 0,
                                     0,   5000.0, 0,     0,      0, 500.0};
  }

  double diag = bboxDiag(bbox);

  // 叶子层：直接挂 Block externalTileset
  // 引用（每块单独作为一个child，使用REPLACE）
  if (level >= maxLevel || blocks.size() == 1) {
    // 叶节点的 geometricError = 包围盒对角线 * scale * 2
    // 这个值必须小于其父节点，确保Cesium在靠近时才展开
    double leafGeoError =
        (diag > 0.0) ? diag * cfg.geometricErrorScale * 2.0 : 1000.0;
    node["geometricError"] = leafGeoError;
    node["refine"] = "REPLACE";

    if (blocks.size() == 1) {
      // 单 Block：直接 content 引用（无 children）
      node["content"]["uri"] = blocks[0]->blockTilesetRelPath;
    } else {
      // 多 Block 但已到最大层：每块作为独立的 child（REPLACE，按距离切换）
      json children = json::array();
      for (const auto *br : blocks) {
        json child;
        if (br->bbox.valid()) {
          float cx = (br->bbox._min.x() + br->bbox._max.x()) * 0.5f;
          float cy = (br->bbox._min.y() + br->bbox._max.y()) * 0.5f;
          float cz = (br->bbox._min.z() + br->bbox._max.z()) * 0.5f;
          float hx = (br->bbox._max.x() - br->bbox._min.x()) * 0.5f;
          float hy = (br->bbox._max.y() - br->bbox._min.y()) * 0.5f;
          float hz = (br->bbox._max.z() - br->bbox._min.z()) * 0.5f;
          child["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0,
                                            0,  hy, 0,  0,  0, hz};
        } else {
          child["boundingVolume"]["box"] = {
              0.0, 0.0, 200.0, 1000.0, 0, 0, 0, 1000.0, 0, 0, 0, 300.0};
        }
        double childDiag = bboxDiag(br->bbox);
        // 每个Block的geometricError略小于叶节点
        child["geometricError"] = (childDiag > 0.0)
                                      ? childDiag * cfg.geometricErrorScale
                                      : leafGeoError * 0.5;
        child["refine"] = "REPLACE";
        child["content"]["uri"] = br->blockTilesetRelPath;
        children.push_back(child);
      }
      node["children"] = std::move(children);
    }
    return node;
  }

  // 内部节点：按 x/y 中分为最多 4 个象限（REPLACE
  // 模式：按视距切换，不同时加载） 统计 x/y 范围
  int xMin = INT_MAX, xMax = INT_MIN, yMin = INT_MAX, yMax = INT_MIN;
  for (const auto *br : blocks) {
    auto [bx, by] = parseBlockCoord(br->blockName);
    xMin = std::min(xMin, bx);
    xMax = std::max(xMax, bx);
    yMin = std::min(yMin, by);
    yMax = std::max(yMax, by);
  }
  int xMid = (xMin + xMax) / 2;
  int yMid = (yMin + yMax) / 2;

  // 分配到 4 个象限（左下、右下、左上、右上）
  std::array<std::vector<const BlockResult *>, 4> quadrants;
  for (const auto *br : blocks) {
    auto [bx, by] = parseBlockCoord(br->blockName);
    int qi = (bx > xMid ? 1 : 0) + (by > yMid ? 2 : 0);
    quadrants[qi].push_back(br);
  }

  // 检查是否所有 block 都落入同一象限（parseBlockCoord 解析失败时会发生）
  // 若是，则将 blocks 直接均分为4组以避免无限递归
  int nonEmpty = 0;
  for (const auto &q : quadrants)
    if (!q.empty())
      ++nonEmpty;
  if (nonEmpty <= 1 && blocks.size() > 1) {
    // 按索引强制平均分4组
    for (auto &q : quadrants)
      q.clear();
    for (size_t k = 0; k < blocks.size(); ++k)
      quadrants[k % 4].push_back(blocks[k]);
  }

  // 内部节点 geometricError 必须远大于子节点（梯度 = 6倍scale * diag）
  // 确保 Cesium 只在足够近时才展开子节点
  node["geometricError"] =
      (diag > 0.0) ? diag * cfg.geometricErrorScale * 6.0 : 4000.0;
  node["refine"] = "REPLACE"; // REPLACE: 只显示当前LOD层，不同时叠加子节点
  json children = json::array();
  for (const auto &q : quadrants) {
    if (q.empty())
      continue;
    children.push_back(buildQuadTreeNode(q, level + 1, maxLevel, cfg));
  }
  if (!children.empty())
    node["children"] = std::move(children);

  return node;
}

// ────────────────────────────────────────────────────────────────────────────
// main
// ────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
  // ── 命令行解析 ───────────────────────────────────────────────
  cxxopts::Options opts("osgb2tiles", "OSGB 转 3D Tiles 转换工具 v2.1");
  opts.add_options()("i,input", "OSGB 输入目录", cxxopts::value<std::string>())(
      "o,output", "3DTiles 输出目录",
      cxxopts::value<std::string>()->default_value("./output"))(
      "config", "JSON 配置文件路径（含七参数等完整配置）",
      cxxopts::value<std::string>()->default_value(""))(
      "lon", "原点经度（度）", cxxopts::value<double>()->default_value("0.0"))(
      "lat", "原点纬度（度）", cxxopts::value<double>()->default_value("0.0"))(
      "alt", "原点高程（米）", cxxopts::value<double>()->default_value("0.0"))(
      "simplify", "启用网格简化",
      cxxopts::value<bool>()->default_value("false"))(
      "simplify-ratio", "简化比例 [0.1~1.0]",
      cxxopts::value<float>()->default_value("0.5"))(
      "compress-tex", "启用纹理格式转换",
      cxxopts::value<bool>()->default_value("false"))(
      "tex-format", "texture format: ktx2/jpg/png/webp",
      cxxopts::value<std::string>()->default_value("ktx2"))(
      "ktx2-mode", "KTX2 mode: etc1s|uastc",
      cxxopts::value<std::string>()->default_value("etc1s"))(
      "ktx2-quality", "KTX2 compression level [1-5]",
      cxxopts::value<int>()->default_value("2"))(
      "ktx2-mipmaps", "KTX2 generate mipmaps",
      cxxopts::value<bool>()->default_value("true"))(
      "tex-size", "最大纹理尺寸（像素）",
      cxxopts::value<int>()->default_value("2048"))(
      "jpeg-quality", "JPEG 质量 [1-100]",
      cxxopts::value<int>()->default_value("85"))(
      "webp-quality", "WebP 质量 [1-100]",
      cxxopts::value<int>()->default_value("80"))(
      "webp-lossless", "WebP 无损模式",
      cxxopts::value<bool>()->default_value("false"))(
      "draco", "Draco 几何压缩（体积可减~70%）",
      cxxopts::value<bool>()->default_value("false"))(
      "draco-bits", "Draco 位置量化位数[8-16]",
      cxxopts::value<int>()->default_value("14"))(
      "geo-error", "geometricError 系数[0.1~2.0]",
      cxxopts::value<double>()->default_value("0.5"))(
      "merge-level", "根节点合并层级: -1=自动, 0=不合并, N=强制N层",
      cxxopts::value<int>()->default_value("-1"))(
      "merge-root", "真正的根节点合并（生成简化几何体，提升远景加载效率）",
      cxxopts::value<bool>()->default_value("false"))(
      "format", "输出格式: b3dm / glb",
      cxxopts::value<std::string>()->default_value("b3dm"))(
      "threads", "并行线程数（0=自动）",
      cxxopts::value<int>()->default_value("4"))(
      "v,verbose", "详细日志",
      cxxopts::value<bool>()->default_value("false"))("h,help", "显示帮助");

  cxxopts::ParseResult result;
  try {
    result = opts.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n" << opts.help() << "\n";
    return 1;
  }

  if (result.count("help") || !result.count("input")) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  // ── 自动设置 PROJ_DATA（使 GDAL 能找到 proj.db，支持任意坐标系）────────
  // 如果用户已在环境中设置，跳过；否则搜索 exe 同目录下的候选路径
  bool projDataSet = (std::getenv("PROJ_DATA") != nullptr) ||
                     (std::getenv("PROJ_LIB") != nullptr);
  if (!projDataSet) {
    // 获取 exe 所在目录
    fs::path exeDir;
    try {
#if defined(_WIN32)
      char buf[4096];
      DWORD len = GetModuleFileNameA(nullptr, buf, 4096);
      if (len > 0)
        exeDir = fs::path(buf).parent_path();
#elif defined(__linux__)
      char buf[4096];
      ssize_t len = readlink("/proc/self/exe", buf, 4096);
      if (len > 0) {
        buf[len] = 0;
        exeDir = fs::path(buf).parent_path();
      }
#endif
    } catch (...) {
    }

    const std::vector<std::string> projCandidates = {
        "proj",             // <exeDir>/proj/
        "share/proj",       // <exeDir>/share/proj/
        "../share/proj",    // <exeDir>/../share/proj/
        "../../share/proj", // build/bin/../../share/proj/
    };
    for (const auto &rel : projCandidates) {
      fs::path candidate = exeDir / rel;
      if (fs::exists(candidate / "proj.db")) {
        std::string projPath = candidate.string();
#if defined(_WIN32)
        _putenv_s("PROJ_DATA", projPath.c_str());
        _putenv_s("PROJ_LIB", projPath.c_str());
#else
        setenv("PROJ_DATA", projPath.c_str(), 1);
        setenv("PROJ_LIB", projPath.c_str(), 1);
#endif
        std::cout << "[INFO] PROJ_DATA auto-set to: " << projPath << "\n";
        projDataSet = true;
        break;
      }
    }
    if (!projDataSet) {
      std::cout
          << "[WARN] proj.db not found. GDAL coordinate transform may fail.\n"
          << "       将 proj/ 目录（含 proj.db）放置在 osgb2tiles.exe "
             "同目录下\n";
    }
  }

  ConvertConfig cfg;
  std::string cfgPath = result["config"].as<std::string>();
  if (!cfgPath.empty() && fs::exists(cfgPath))
    cfg = loadJsonConfig(cfgPath);

  // ── CLI 参数只在用户明确传入时才覆盖 JSON 配置 ──────────────
  // 注意：input/output 是必需参数，始终应用
  cfg.inputPath = result["input"].as<std::string>();
  if (result.count("output"))
    cfg.outputPath = result["output"].as<std::string>();
  if (result.count("simplify"))
    cfg.simplifyMesh = result["simplify"].as<bool>();
  if (result.count("simplify-ratio"))
    cfg.simplifyRatio = result["simplify-ratio"].as<float>();
  if (result.count("compress-tex"))
    cfg.compressTexture = result["compress-tex"].as<bool>();
  if (result.count("tex-format"))
    cfg.textureFormat = result["tex-format"].as<std::string>();
  if (result.count("tex-size"))
    cfg.maxTextureSize = result["tex-size"].as<int>();
  if (result.count("jpeg-quality"))
    cfg.jpegQuality = result["jpeg-quality"].as<int>();
  if (result.count("webp-quality"))
    cfg.webpQuality = result["webp-quality"].as<int>();
  if (result.count("webp-lossless"))
    cfg.webpLossless = result["webp-lossless"].as<bool>();
  if (result.count("ktx2-mode"))
    cfg.ktx2Mode = result["ktx2-mode"].as<std::string>();
  if (result.count("ktx2-quality"))
    cfg.ktx2Quality = result["ktx2-quality"].as<int>();
  if (result.count("ktx2-mipmaps"))
    cfg.ktx2Mipmaps = result["ktx2-mipmaps"].as<bool>();
  if (result.count("draco"))
    cfg.compressGeometry = result["draco"].as<bool>();
  if (result.count("draco-bits"))
    cfg.dracoQuantBits = result["draco-bits"].as<int>();
  if (result.count("geo-error"))
    cfg.geometricErrorScale = result["geo-error"].as<double>();
  if (result.count("merge-level"))
    cfg.mergeLevel = result["merge-level"].as<int>();
  if (result.count("merge-root"))
    cfg.mergeRoot = result["merge-root"].as<bool>();
  if (result.count("format"))
    cfg.tileFormat = result["format"].as<std::string>();
  if (result.count("verbose") || result.count("v"))
    cfg.verbose = true;

  // ── 地理坐标原点 ─────────────────────────────────────────────
  double argLon = result["lon"].as<double>();
  double argLat = result["lat"].as<double>();
  double argAlt = result["alt"].as<double>();
  bool userCoords = (argLon != 0.0 || argLat != 0.0);

  Logger::instance().setVerbose(cfg.verbose);

  if (userCoords) {
    cfg.longitude = argLon;
    cfg.latitude = argLat;
    cfg.height = argAlt;
  } else {
    LOG_INFO("lon/lat not specified, scanning for metadata files...");
    auto origin = OsgbMetaReader::tryRead(cfg.inputPath);
    if (origin) {
      cfg.longitude = origin->longitude;
      cfg.latitude = origin->latitude;
      cfg.height = origin->height;
      LOG_INFO(
          "Auto-detected coordinates: lon=" + std::to_string(cfg.longitude) +
          " lat=" + std::to_string(cfg.latitude) +
          " alt=" + std::to_string(cfg.height));
    } else {
      std::cerr << "\n[ERROR] 无法自动获取地理坐标原点。\n"
                << "        请通过 --lon/--lat/--alt 手动指定\n\n";
      return 1;
    }
  }

  int threadArg = result["threads"].as<int>();
  cfg.threads = (threadArg <= 0)
                    ? static_cast<int>(std::thread::hardware_concurrency())
                    : threadArg;

  // ── 打印配置 ─────────────────────────────────────────────────
  LOG_INFO("=== osgb2tiles v2.0 ===");
  LOG_INFO("Input   : " + cfg.inputPath);
  LOG_INFO("Output  : " + cfg.outputPath);
  LOG_INFO("Origin  : lon=" + std::to_string(cfg.longitude) + " lat=" +
           std::to_string(cfg.latitude) + " alt=" + std::to_string(cfg.height));
  LOG_INFO("Format  : " + cfg.tileFormat);
  LOG_INFO("Threads : " + std::to_string(cfg.threads));

  auto t0 = std::chrono::steady_clock::now();

  // ── 扫描找所有根瓦片 .osgb ───────────────────────────────────
  OsgbReader scanner(cfg.verbose);
  auto rootNodes = scanner.scan(cfg.inputPath);

  if (rootNodes.empty()) {
    LOG_ERROR("No OSGB tiles found in: " + cfg.inputPath);
    return 1;
  }
  LOG_INFO("Found " + std::to_string(rootNodes.size()) + " root blocks.");

  // ── 创建输出目录 ─────────────────────────────────────────────
  fs::create_directories(cfg.outputPath);
  fs::create_directories(cfg.outputPath + "/Data");

  // ── 计算 ENU→ECEF 变换矩阵（写入 tileset.json 根节点的 transform 字段）──
  GeometryConverter geoConv(cfg);
  auto xfm = geoConv.getTransformMatrix();
  std::vector<double> xfmVec(xfm.begin(), xfm.end());

  // ── 逐 Block 处理 ─────────────────────────────────────────────
  ThreadPool pool(static_cast<size_t>(cfg.threads));
  std::atomic<int> totalConverted{0};
  std::mutex logMtx;

  std::vector<BlockResult> blockResults;
  int blockIdx = 0;
  for (const auto &rn : rootNodes) {
    ++blockIdx;
    LOG_INFO("[Block " + std::to_string(blockIdx) + "/" +
             std::to_string(rootNodes.size()) + "] Processing: " + rn.nodeId);
    auto br = processBlock(rn.osgbPath, cfg.outputPath, cfg, pool,
                           totalConverted, logMtx);
    blockResults.push_back(std::move(br));
  }

  // ── 生成全局 tileset.json ─────────────────────────────────────
  json globalTs;
  globalTs["asset"]["version"] = "1.0";
  globalTs["asset"]["gltfUpAxis"] = "Z";
  // 全局 tileset geometricError 在计算 globalBbox 之后再覆写（P2）

  // ── 计算全局根节点的包围盒（从所有 Block 的 bbox 合并）────────────────
  // 注意：bbox 坐标为 OSGB 局部坐标系（与顶点坐标相同空间），
  //       由根节点的 transform 矩阵（ENU→ECEF）统一映射到世界坐标。
  osg::BoundingBox globalBbox;
  for (const auto &br : blockResults) {
    if (br.ok && br.bbox.valid()) {
      globalBbox.expandBy(br.bbox._min);
      globalBbox.expandBy(br.bbox._max);
    }
  }

  json rootBV;
  if (globalBbox.valid()) {
    float cx = (globalBbox._min.x() + globalBbox._max.x()) * 0.5f;
    float cy = (globalBbox._min.y() + globalBbox._max.y()) * 0.5f;
    float cz = (globalBbox._min.z() + globalBbox._max.z()) * 0.5f;
    float hx = (globalBbox._max.x() - globalBbox._min.x()) * 0.5f +
               100.0f; // +100m padding
    float hy = (globalBbox._max.y() - globalBbox._min.y()) * 0.5f + 100.0f;
    float hz = (globalBbox._max.z() - globalBbox._min.z()) * 0.5f + 50.0f;
    rootBV["box"] = {cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz};
  } else {
    // 没有有效 bbox 时的兜底（理论上不应触发）
    rootBV["box"] = {0.0, 0.0, 200.0, 5000.0, 0, 0, 0, 5000.0, 0, 0, 0, 500.0};
  }

  // 全局根 geometricError：基于 globalBbox 对角线（P2）
  double globalDiag = bboxDiag(globalBbox);
  double globalGeoError =
      (globalDiag > 0.0) ? globalDiag * cfg.geometricErrorScale * 4.0 : 32768.0;
  globalTs["geometricError"] = globalGeoError;

  json globalRoot;
  globalRoot["transform"] = xfmVec;
  globalRoot["boundingVolume"] = rootBV;
  // 根节点 geometricError 保持较大值，确保 Cesium 从全局视角只展开第一层
  globalRoot["geometricError"] = globalGeoError * 0.5;
  globalRoot["refine"] =
      "REPLACE"; // 改为REPLACE：按视距切换层级，不同时加载全部

  // ── 根节点合并层级（自动计算）─────────────────────────────────────────
  int totalBlocks = static_cast<int>(blockResults.size());
  int mergeLevel = cfg.mergeLevel;
  if (mergeLevel < 0) {
    mergeLevel = autoComputeMergeLevel(totalBlocks);
    LOG_INFO("智能根节点合并：总块数=" + std::to_string(totalBlocks) +
             "，自动计算合并层级=" + std::to_string(mergeLevel) +
             "（目标根节点数 <= " +
             std::to_string(static_cast<int>(
                 std::ceil(totalBlocks / std::pow(4.0, mergeLevel)))) +
             "）");
  } else {
    LOG_INFO("根节点合并：总块数=" + std::to_string(totalBlocks) +
             "，合并层级=" + std::to_string(mergeLevel) +
             (mergeLevel == 0 ? "（不合并）" : ""));
  }

  // 收集成功的 BlockResult 指针
  std::vector<const BlockResult *> okBlocks;
  for (const auto &br : blockResults) {
    if (br.ok)
      okBlocks.push_back(&br);
  }

  json globalChildren;
  if (mergeLevel == 0 || okBlocks.empty()) {
    // 不合并：平铺所有 Block
    globalChildren = json::array();
    for (const auto *br : okBlocks) {
      json child;
      if (br->bbox.valid()) {
        float cx = (br->bbox._min.x() + br->bbox._max.x()) * 0.5f;
        float cy = (br->bbox._min.y() + br->bbox._max.y()) * 0.5f;
        float cz = (br->bbox._min.z() + br->bbox._max.z()) * 0.5f;
        float hx = (br->bbox._max.x() - br->bbox._min.x()) * 0.5f;
        float hy = (br->bbox._max.y() - br->bbox._min.y()) * 0.5f;
        float hz = (br->bbox._max.z() - br->bbox._min.z()) * 0.5f;
        child["boundingVolume"]["box"] = {cx, cy, cz, hx, 0, 0,
                                          0,  hy, 0,  0,  0, hz};
      } else {
        child["boundingVolume"]["box"] = {0.0, 0.0,    200.0, 1000.0, 0, 0,
                                          0,   1000.0, 0,     0,      0, 300.0};
      }
      double childDiag = bboxDiag(br->bbox);
      child["geometricError"] = (childDiag > 0.0)
                                    ? childDiag * cfg.geometricErrorScale * 2.0
                                    : 1000.0;
      child["refine"] = "REPLACE";
      child["content"]["uri"] = br->blockTilesetRelPath;
      globalChildren.push_back(child);
    }
  } else {
    // 合并模式：构建四叉树
    json quadRoot = buildQuadTreeNode(okBlocks, 0, mergeLevel, cfg);
    // 取四叉树子节点作为 globalRoot 的 children
    if (quadRoot.contains("children")) {
      globalChildren = quadRoot["children"];
    } else {
      // 四叉树只有一个节点（所有 block 落在同一象限）
      globalChildren = json::array();
      globalChildren.push_back(quadRoot);
    }
  }
  globalRoot["children"] = std::move(globalChildren);
  globalTs["root"] = globalRoot;

  std::string globalTilesetPath = cfg.outputPath + "/tileset.json";
  std::ofstream gofs(globalTilesetPath);
  if (gofs) {
    gofs << globalTs.dump(2);
    LOG_INFO("Wrote global tileset.json -> " + globalTilesetPath);
  }

  // ── 真正的根节点合并（可选） ───────────────────────────────────
  if (cfg.mergeRoot && okBlocks.size() > 1) {
    LOG_INFO("=== 执行真正的根节点合并 ===");
    std::vector<MergeBlockInfo> mergeInfos;
    std::string ext = (cfg.tileFormat == "b3dm") ? ".b3dm" : ".glb";
    for (const auto &br : blockResults) {
      if (!br.ok)
        continue;
      MergeBlockInfo mi;
      mi.blockName = br.blockName;
      mi.blockTilesetRelPath = br.blockTilesetRelPath;
      mi.bbox = br.bbox;
      // 计算 Block 根节点的 geometricError
      double d = bboxDiag(br.bbox);
      mi.geometricError = (d > 0.0) ? d * cfg.geometricErrorScale * 2.0 : 6.0;
      // 找到 Block 根瓦片文件路径
      std::string blockDirName =
          fs::path(br.blockTilesetRelPath).parent_path().filename().string();
      mi.rootTilePath =
          cfg.outputPath + "/Data/" + blockDirName + "/" + blockDirName + ext;
      mergeInfos.push_back(std::move(mi));
    }
    TopLevelMerger merger;
    if (!merger.merge(mergeInfos, cfg, cfg.outputPath)) {
      LOG_WARN("根节点合并失败，但基础转换数据完好");
    }
  }

  // ── 完成报告 ─────────────────────────────────────────────────
  auto t1 = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  int succeededBlocks = 0;
  for (const auto &br : blockResults)
    if (br.ok)
      ++succeededBlocks;

  LOG_INFO("=== 转换完成 ===");
  LOG_INFO("成功块: " + std::to_string(succeededBlocks) + "/" +
           std::to_string(blockResults.size()) +
           "  总转换节点: " + std::to_string(totalConverted) +
           "  总耗时: " + std::to_string(static_cast<int>(elapsed)) + "s");
  LOG_INFO("输出目录: " + cfg.outputPath);

  return succeededBlocks > 0 ? 0 : 1;
}
