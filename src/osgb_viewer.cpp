/**
 * osgb_viewer.cpp v3.1 — DasViewer 风格 OSGB 三维浏览器
 *
 * 架构: Win32 UI + OSG 渲染，OSG 独立窗口通过 SetParent 嵌入到 Win32 子 HWND
 *
 * 关键: Win32 头文件必须早于 OSG 头文件包含，且不使用 WIN32_LEAN_AND_MEAN
 * 以确保 commctrl.h 依赖的所有 Win32 类型（CALLBACK 等）正确定义
 */

#define _USE_MATH_DEFINES
// ═══ [步骤1] 先包含 Windows 头，不使用 WIN32_LEAN_AND_MEAN ═══════════════
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <windows.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ═══ [步骤2] 再包含 OSG 头文件 ═══════════════════════════════════════════
#undef max
#undef min
#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Light>
#include <osg/LightSource>
#include <osg/MatrixTransform>
#include <osg/Node>
#include <osg/NodeCallback>
#include <osg/PolygonMode>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Vec4>
#include <osgDB/DatabasePager>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>
#include <osgText/Text>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgViewer/api/Win32/GraphicsWindowWin32>

// ═══ [步骤3] STL ══════════════════════════════════════════════════════════
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ════════════════ 布局常量 ════════════════════════════════════════════════
static const int SIDEBAR_W = 220;
static const int TOOLBAR_H = 44;
static const int SB_H = 22;
static const int BTN_W = 70;
static const int BTN_GAP = 3;

// 命令 ID
#define ID_FILE_OPEN 1001
#define ID_FILE_EXIT 1002
#define ID_VIEW_HOME 1101
#define ID_VIEW_WIRE 1102
#define ID_VIEW_LIGHT 1103
#define ID_SHOW_HUD 1201
#define ID_SHOW_TIPS 1202
#define ID_HELP_ABOUT 1301
#define ID_ORTHO_EXPORT 1401
#define ID_ORTHO_DOM 1402
#define ID_PANO_EXPORT 1501
#define IDC_STATUS 9001

// 深色主题色
static const COLORREF C_BG = RGB(37, 37, 38);
static const COLORREF C_TOOL = RGB(28, 28, 30);
static const COLORREF C_SIDE = RGB(43, 43, 46);
static const COLORREF C_HDR = RGB(50, 50, 54);
static const COLORREF C_TXT = RGB(204, 204, 204);
static const COLORREF C_DIM = RGB(120, 120, 125);
static const COLORREF C_ACCENT = RGB(0, 120, 212);
static const COLORREF C_BORDER = RGB(62, 62, 66);
static const COLORREF C_HOVER = RGB(58, 58, 62);
static const COLORREF C_ON = RGB(0, 100, 180);
static HBRUSH hBrBg = nullptr, hBrTool = nullptr, hBrSide = nullptr;

// ════════════════ 全局状态 ════════════════════════════════════════════════
struct {
  HINSTANCE hInst = nullptr;
  HWND hMain = nullptr;
  HWND hTool = nullptr;
  HWND hSide = nullptr;
  HWND hVP = nullptr;     // 视口占位 HWND
  HWND hOsgWin = nullptr; // OSG 自己创建的 HWND（会被 SetParent 到 hVP）
  HWND hStat = nullptr;
  HMENU hMenu = nullptr;
  int btnHover = -1;

  osgViewer::Viewer *viewer = nullptr;
  osg::ref_ptr<osgGA::TrackballManipulator>
      manip; // ref_ptr 防止被 setCameraManipulator(nullptr) 释放
  osg::ref_ptr<osg::Group> sceneRoot;
  osg::ref_ptr<osg::Camera> hudInfo;
  osg::ref_ptr<osg::Camera> hudTips;
  osg::ref_ptr<osg::Node> skyDome; // 背景相机节点（PRE_RENDER）

  bool hudInfoOn = true;
  bool hudTipsOn = true;
  bool wireOn = false;
  bool lightOn = true;

  std::string dataDir;
  int tileCount = 0;
  bool metaValid = false;
  double metaLon = 0, metaLat = 0, metaH = 0;
  std::string metaSrs;

  int vpW = 1300, vpH = 860;

  struct TBtn {
    int id;
    const wchar_t *label;
    bool *toggle;
  };
  std::vector<TBtn> btns;
} G;

// ════════════════ UTF 辅助 ════════════════════════════════════════════════
static std::wstring W(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring r(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
  return r;
}
static std::string U(const std::wstring &w) {
  if (w.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string r(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n,
                      nullptr, nullptr);
  return r;
}

// ════════════════ XML / 元数据 ════════════════════════════════════════════
static std::string xTag(const std::string &xml, const std::string &tag) {
  auto op = xml.find("<" + tag);
  if (op == std::string::npos)
    return {};
  auto gt = xml.find('>', op);
  if (gt == std::string::npos)
    return {};
  auto cl = xml.find("</" + tag + ">", gt + 1);
  if (cl == std::string::npos)
    return {};
  return xml.substr(gt + 1, cl - gt - 1);
}
static std::string xAttr(const std::string &xml, const std::string &tag,
                         const std::string &attr) {
  auto op = xml.find("<" + tag);
  if (op == std::string::npos)
    return {};
  auto gt = xml.find('>', op);
  if (gt == std::string::npos)
    return {};
  std::string el = xml.substr(op, gt - op + 1);
  auto kp = el.find(attr + "=");
  if (kp == std::string::npos)
    return {};
  kp += attr.size() + 1;
  char q = el[kp];
  if (q != '"' && q != '\'')
    return {};
  auto ep = el.find(q, kp + 1);
  if (ep == std::string::npos)
    return {};
  return el.substr(kp + 1, ep - kp - 1);
}
static std::string readFile(const std::string &p) {
  std::ifstream f(p);
  if (!f)
    return {};
  return {std::istreambuf_iterator<char>(f), {}};
}
static void readMeta(const std::string &dir) {
  G.metaValid = false;
  const std::vector<std::pair<std::string, int>> cands = {
      {"metadata.xml", 1},
      {"Metadata.xml", 1},
      {"production_meta.xml", 2},
      {"config.xml", 2},
      {"doc.xml", 3}};
  std::vector<std::string> dirs = {dir};
  try {
    for (auto &e : fs::directory_iterator(dir))
      if (e.is_directory())
        dirs.push_back(e.path().string());
  } catch (...) {
  }
  for (auto &sd : dirs) {
    for (auto &[fn, tp] : cands) {
      std::string p = sd + "/" + fn;
      if (!fs::exists(p))
        continue;
      std::string xml = readFile(p);
      if (xml.empty())
        continue;
      if (tp == 1) {
        std::string orig = xTag(xml, "SRSOrigin"), srs = xTag(xml, "SRS");
        if (!orig.empty()) {
          for (auto &c : orig)
            if (c == ',')
              c = ' ';
          std::istringstream ss(orig);
          double x, y, z = 0;
          if (ss >> x >> y) {
            ss >> z;
            bool wgs = srs.empty() || srs.find("4326") != std::string::npos ||
                       srs.find("WGS") != std::string::npos;
            G.metaLon = x;
            G.metaLat = y;
            G.metaH = z;
            G.metaSrs = srs;
            G.metaValid = true;
            if (!wgs)
              G.metaSrs += u8"（投影坐标）";
            return;
          }
        }
      } else if (tp == 2) {
        std::string lon = xAttr(xml, "Center", "longitude"),
                    lat = xAttr(xml, "Center", "latitude");
        if (lon.empty()) {
          lon = xTag(xml, "center_longitude");
          lat = xTag(xml, "center_latitude");
        }
        if (!lon.empty() && !lat.empty()) {
          try {
            G.metaLon = std::stod(lon);
            G.metaLat = std::stod(lat);
            G.metaSrs = "WGS84";
            G.metaValid = true;
            return;
          } catch (...) {
          }
        }
      } else if (tp == 3) {
        std::string lon = xAttr(xml, "reference", "x"),
                    lat = xAttr(xml, "reference", "y");
        if (!lon.empty() && !lat.empty()) {
          try {
            G.metaLon = std::stod(lon);
            G.metaLat = std::stod(lat);
            G.metaSrs = "WGS84";
            G.metaValid = true;
            return;
          } catch (...) {
          }
        }
      }
    }
  }
}

// ════════════════ 瓦片扫描 ════════════════════════════════════════════════
static std::vector<std::string> scanTiles(const std::string &dir) {
  std::vector<std::string> roots;
  if (fs::is_regular_file(dir)) {
    roots.push_back(dir);
    return roots;
  }
  fs::path dataDir = fs::path(dir) / "Data";
  if (fs::is_directory(dataDir)) {
    try {
      for (auto &td : fs::directory_iterator(dataDir)) {
        if (!td.is_directory())
          continue;
        fs::path rp = td.path() / (td.path().filename().string() + ".osgb");
        if (fs::is_regular_file(rp)) {
          roots.push_back(rp.string());
          continue;
        }
        for (auto &f : fs::directory_iterator(td.path())) {
          if (!f.is_regular_file())
            continue;
          auto ext = f.path().extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          if (ext == ".osgb" || ext == ".osg") {
            roots.push_back(f.path().string());
            break;
          }
        }
      }
    } catch (...) {
    }
  }
  if (roots.empty()) {
    try {
      for (auto &f : fs::directory_iterator(dir)) {
        if (!f.is_regular_file())
          continue;
        auto ext = f.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".osgb" || ext == ".osg" || ext == ".ive")
          roots.push_back(f.path().string());
      }
    } catch (...) {
    }
  }
  std::sort(roots.begin(), roots.end());
  return roots;
}

// ════════════════ OSG HUD ════════════════════════════════════════════════
static osg::Camera *makeHudCam(int w, int h, int order) {
  auto *cam = new osg::Camera();
  cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
  cam->setProjectionMatrixAsOrtho2D(0, w, 0, h);
  cam->setViewMatrix(osg::Matrix::identity());
  cam->setRenderOrder(osg::Camera::POST_RENDER, order);
  cam->setClearMask(GL_DEPTH_BUFFER_BIT);
  cam->setAllowEventFocus(false);
  cam->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
  return cam;
}
static osgText::Text *makeText(float sz, osg::Vec4 col) {
  auto *t = new osgText::Text();
  t->setCharacterSize(sz);
  t->setColor(col);
  t->setDataVariance(osg::Object::DYNAMIC);
  t->setBackdropType(osgText::Text::DROP_SHADOW_BOTTOM_RIGHT);
  t->setBackdropColor(osg::Vec4(0, 0, 0, .75f));
  return t;
}
static osg::Camera *buildInfoHud(int w, int h) {
  auto *cam = makeHudCam(w, h, 0);
  auto *geode = new osg::Geode();
  auto *text = makeText(13.f, osg::Vec4(.4f, .95f, .4f, .92f));
  text->setPosition(osg::Vec3(10.f, h - 18.f, 0));
  text->setName("info");
  geode->addDrawable(text);
  cam->addChild(geode);
  return cam;
}
static osg::Camera *buildTipsHud(int w, int h) {
  auto *cam = makeHudCam(w, h, 1);
  auto *geode = new osg::Geode();
  auto *text = makeText(12.f, osg::Vec4(.85f, .85f, .85f, .55f));
  text->setAlignment(osgText::Text::RIGHT_BOTTOM);
  text->setPosition(osg::Vec3((float)(w - 10), 12.f, 0));
  text->setText(
      "[F] \u590d\u4f4d  [H] \u4fe1\u606fHUD  [T] \u63d0\u793a  [W] "
      "\u7ebf\u6846  [L] \u706f\u5149  [ESC] \u9000\u51fa\n"
      "\u5de6\u952e:\u65cb\u8f6c  \u4e2d\u952e/Shift+\u5de6:\u5e73\u79fb  "
      "\u6eda\u8f6e:\u7f29\u653e");
  text->setName("tips");
  geode->addDrawable(text);
  cam->addChild(geode);
  return cam;
}
static void updateInfoHud() {
  if (!G.hudInfo || !G.viewer)
    return;
  auto *geode = dynamic_cast<osg::Geode *>(G.hudInfo->getChild(0));
  if (!geode)
    return;
  auto *text = dynamic_cast<osgText::Text *>(geode->getDrawable(0));
  if (!text)
    return;
  auto *pager = G.viewer->getDatabasePager();
  int pending = pager ? pager->getRequestsInProgress() : 0;
  double fps = 0;
  const auto *s = G.viewer->getFrameStamp();
  if (s && s->getFrameNumber() > 1 && s->getReferenceTime() > 0)
    fps = s->getFrameNumber() / s->getReferenceTime();
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1);
  if (!G.dataDir.empty())
    ss << fs::path(G.dataDir).filename().string() << "\n";
  ss << "FPS:" << fps << "  \u74e6\u7247:" << G.tileCount
     << "  \u52a0\u8f7d\u4e2d:" << pending;
  if (G.metaValid) {
    ss << std::setprecision(6) << "\n\u5750\u6807:" << G.metaLon << "°, "
       << G.metaLat << "°  H=" << std::setprecision(1) << G.metaH << "m";
  }
  text->setText(ss.str());
}

// ════════════════ 调试文件日志 ════════════════════════════════════════════
static FILE *s_logFile = nullptr;
static void FLOG(const char *fmt, ...) {
  if (!s_logFile) {
    s_logFile = fopen("viewer_debug.log", "w");
    if (!s_logFile)
      return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(s_logFile, fmt, ap);
  va_end(ap);
  fflush(s_logFile);
}

// ════════════════ 全局崩溃捕获器 ════════════════════════════════════════
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep) {
  FLOG("[CRASH] Unhandled exception! Code=0x%08X Addr=%p\n",
       ep->ExceptionRecord->ExceptionCode,
       ep->ExceptionRecord->ExceptionAddress);
  if (s_logFile)
    fclose(s_logFile);
  s_logFile = nullptr;
  return EXCEPTION_CONTINUE_SEARCH;
}

// 导出状态标志（提前定义，后续 KeyHandler 和 MainProc 需要引用）
static bool s_exporting = false;

// ★★★ 核武器退出守卫 ★★★
// 只有用户明确操作（ESC/菜单退出/关闭窗口）才设为 true
// 主循环仅凭此标志决定是否退出，OSG 内部任何 viewer->done()=true 都被强制重置
static bool s_userWantsQuit = false;

// ════════════════ 键盘处理器 ══════════════════════════════════════════════
class KeyHandler : public osgGA::GUIEventHandler {
  osg::Node *_scene;

public:
  KeyHandler(osg::Node *s) : _scene(s) {}
  bool handle(const osgGA::GUIEventAdapter &ea,
              osgGA::GUIActionAdapter &aa) override {
    if (ea.getEventType() != osgGA::GUIEventAdapter::KEYDOWN)
      return false;
    int k = ea.getKey();
    auto *v = dynamic_cast<osgViewer::Viewer *>(&aa);
    if (k == 'f' || k == 'F') {
      if (G.manip)
        G.manip->home(0);
      return true;
    }
    if (k == 'h' || k == 'H') {
      G.hudInfoOn = !G.hudInfoOn;
      if (G.hudInfo)
        G.hudInfo->setNodeMask(G.hudInfoOn ? ~0u : 0u);
      if (G.hTool)
        InvalidateRect(G.hTool, nullptr, FALSE);
      if (G.hSide)
        InvalidateRect(G.hSide, nullptr, FALSE);
      return true;
    }
    if (k == 't' || k == 'T') {
      G.hudTipsOn = !G.hudTipsOn;
      if (G.hudTips)
        G.hudTips->setNodeMask(G.hudTipsOn ? ~0u : 0u);
      if (G.hTool)
        InvalidateRect(G.hTool, nullptr, FALSE);
      if (G.hSide)
        InvalidateRect(G.hSide, nullptr, FALSE);
      return true;
    }
    if (k == 'w' || k == 'W') {
      G.wireOn = !G.wireOn;
      if (_scene) {
        auto *pm = new osg::PolygonMode();
        pm->setMode(osg::PolygonMode::FRONT_AND_BACK,
                    G.wireOn ? osg::PolygonMode::LINE : osg::PolygonMode::FILL);
        _scene->getOrCreateStateSet()->setAttribute(pm);
      }
      if (G.hTool)
        InvalidateRect(G.hTool, nullptr, FALSE);
      if (G.hSide)
        InvalidateRect(G.hSide, nullptr, FALSE);
      return true;
    }
    if (k == 'l' || k == 'L') {
      G.lightOn = !G.lightOn;
      if (_scene)
        _scene->getOrCreateStateSet()->setMode(
            GL_LIGHTING,
            G.lightOn ? osg::StateAttribute::ON : osg::StateAttribute::OFF);
      if (G.hTool)
        InvalidateRect(G.hTool, nullptr, FALSE);
      if (G.hSide)
        InvalidateRect(G.hSide, nullptr, FALSE);
      return true;
    }
    if (k == osgGA::GUIEventAdapter::KEY_Escape) {
      if (s_exporting)
        return true; // 导出期间忽略 ESC
      FLOG("[EXIT] ESC key pressed, setting s_userWantsQuit\n");
      s_userWantsQuit = true; // 核武器方案：仅设标志
      return true;
    }
    return false;
  }
};

// ════════════════ 正射输出 ════════════════════════════════════════════════

// 手写无压缩 Baseline TIFF 6.0
static bool writeTiff(const std::string &path, const unsigned char *rgb, int w,
                      int h) {
  std::ofstream out(W(path), std::ios::binary);
  if (!out)
    return false;

  auto w16 = [&](uint16_t v) { out.write((char *)&v, 2); };
  auto w32 = [&](uint32_t v) { out.write((char *)&v, 4); };

  uint32_t stripBytes = (uint32_t)w * h * 3;
  // 各数据块偏移: header(8) + numEntries(2) + 12*12 + nextIFD(4) = 158
  uint32_t bpsOff = 158;           // BitsPerSample 3×SHORT
  uint32_t xresOff = bpsOff + 6;   // XRes RATIONAL
  uint32_t yresOff = xresOff + 8;  // YRes RATIONAL
  uint32_t stripOff = yresOff + 8; // 像素数据

  // TIFF Header: little-endian, magic 42, IFD offset=8
  w16(0x4949);
  w16(42);
  w32(8);

  // IFD: 12 个条目
  w16(12);
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t cnt, uint32_t val) {
    w16(tag);
    w16(type);
    w32(cnt);
    w32(val);
  };
  entry(256, 4, 1, (uint32_t)w); // ImageWidth
  entry(257, 4, 1, (uint32_t)h); // ImageLength
  entry(258, 3, 3, bpsOff);      // BitsPerSample → offset
  entry(259, 3, 1, 1);           // Compression = None
  entry(262, 3, 1, 2);           // PhotometricInterp = RGB
  entry(273, 4, 1, stripOff);    // StripOffsets
  entry(277, 3, 1, 3);           // SamplesPerPixel
  entry(278, 4, 1, (uint32_t)h); // RowsPerStrip
  entry(279, 4, 1, stripBytes);  // StripByteCounts
  entry(282, 5, 1, xresOff);     // XResolution → offset
  entry(283, 5, 1, yresOff);     // YResolution → offset
  entry(296, 3, 1, 2);           // ResolutionUnit = inch
  w32(0);                        // next IFD = 0

  // BitsPerSample data: 8 8 8
  w16(8);
  w16(8);
  w16(8);
  // XResolution: 72/1
  w32(72);
  w32(1);
  // YResolution: 72/1
  w32(72);
  w32(1);

  // 像素数据 — tileBuf 已经是 top→bottom（与 TIFF 行序一致）
  out.write((const char *)rgb, (size_t)w * h * 3);

  return out.good();
}

// TFW 世界文件
static void writeTfw(const std::string &path, double left, double top,
                     double pxW, double pxH) {
  std::ofstream f(W(path));
  if (!f)
    return;
  f << std::fixed << std::setprecision(10) << pxW << "\n0\n0\n"
    << -pxH << "\n"
    << (left + pxW * 0.5) << "\n"
    << (top - pxH * 0.5) << "\n";
}

// PRJ 投影文件
static void writePrj(const std::string &path, const std::string &srs) {
  std::ofstream f(W(path));
  if (!f)
    return;
  if (srs.find("4326") != std::string::npos ||
      srs.find("WGS") != std::string::npos)
    f << "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\","
         "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
         "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]]";
  else
    f << "LOCAL_CS[\"" << srs << "\"]";
}

// 保存文件对话框
static std::string pickSaveTif() {
  wchar_t buf[MAX_PATH] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = G.hMain;
  ofn.lpstrFilter = L"TIFF (*.tif)\0*.tif\0\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"tif";
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrTitle = L"\u4fdd\u5b58\u6b63\u5c04\u56fe\u50cf";
  if (GetSaveFileNameW(&ofn))
    return U(buf);
  return {};
}

// \u2500\u2500 \u5730\u9762\u5206\u8fa8\u7387\u5bf9\u8bdd\u6846
// \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
static double s_orthoGSD =
    0.5; // \u5730\u9762\u5206\u8fa8\u7387\uff08\u7c73/\u50cf\u7d20\uff09

static INT_PTR CALLBACK OrthoDlgProc(HWND hd, UINT msg, WPARAM wp, LPARAM) {
  switch (msg) {
  case WM_INITDIALOG:
    SetDlgItemTextW(hd, 101, L"0.5");
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(wp) == IDOK) {
      wchar_t buf[64] = {};
      GetDlgItemTextW(hd, 101, buf, 64);
      s_orthoGSD = _wtof(buf);
      if (s_orthoGSD < 0.001)
        s_orthoGSD = 0.001;
      if (s_orthoGSD > 1000.0)
        s_orthoGSD = 1000.0;
      EndDialog(hd, IDOK);
      return TRUE;
    }
    if (LOWORD(wp) == IDCANCEL) {
      EndDialog(hd, IDCANCEL);
      return TRUE;
    }
    break;
  case WM_CLOSE:
    EndDialog(hd, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}

static bool showOrthoDlg() {
  alignas(4) BYTE buf[2048] = {};
  BYTE *p = buf;

  auto *dlg = (DLGTEMPLATE *)p;
  dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
  dlg->cdit = 4;
  dlg->cx = 210;
  dlg->cy = 60;
  p += sizeof(DLGTEMPLATE);
  *(WORD *)p = 0;
  p += 2; // menu
  *(WORD *)p = 0;
  p += 2; // class
  const wchar_t *ttl =
      L"\u6b63\u5c04\u8f93\u51fa - \u5730\u9762\u5206\u8fa8\u7387";
  size_t tl = (wcslen(ttl) + 1) * 2;
  memcpy(p, ttl, tl);
  p += tl;

  auto addCtrl = [&](DWORD st, short x, short y, short cx, short cy, WORD id,
                     WORD cls, const wchar_t *txt) {
    p = (BYTE *)(((uintptr_t)p + 3) & ~3);
    auto *it = (DLGITEMTEMPLATE *)p;
    it->style = st | WS_CHILD | WS_VISIBLE;
    it->x = x;
    it->y = y;
    it->cx = cx;
    it->cy = cy;
    it->id = id;
    p += sizeof(DLGITEMTEMPLATE);
    *(WORD *)p = 0xFFFF;
    p += 2;
    *(WORD *)p = cls;
    p += 2;
    size_t n = (wcslen(txt) + 1) * 2;
    memcpy(p, txt, n);
    p += n;
    *(WORD *)p = 0;
    p += 2;
  };
  addCtrl(SS_LEFT, 10, 14, 70, 10, (WORD)-1, 0x0082,
          L"\u5730\u9762\u5206\u8fa8\u7387(\u7c73/\u50cf\u7d20):");
  addCtrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 85, 12, 50, 14, 101, 0x0081,
          L"0.5");
  addCtrl(BS_DEFPUSHBUTTON | WS_TABSTOP, 40, 38, 55, 16, IDOK, 0x0080,
          L"\u786e\u5b9a\u5bfc\u51fa");
  addCtrl(WS_TABSTOP, 110, 38, 55, 16, IDCANCEL, 0x0080, L"\u53d6\u6d88");

  return DialogBoxIndirectParamW(G.hInst, (DLGTEMPLATE *)buf, G.hMain,
                                 OrthoDlgProc, 0) == IDOK;
}

// \u2500\u2500 \u6b63\u5c04\u6e32\u67d3\u5e76\u5bfc\u51fa
// \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
// s_exporting 已在前面定义
static bool s_orthoActive = false;
static bool s_orthoNadir = false;
static bool s_orthoJustDone = false;
static std::wstring s_orthoDoneMsg;
static int s_orthoFrame = 0;
static int s_orthoTotal = 30;
static double s_subCenterX, s_subCenterY; // 子渲染中心（相对于 sceneCenter）
static double s_near, s_far;
static osg::Matrixd s_savedProj;

// ══ 两级分块状态 ══
static const int MAX_TILE_PX = 8192;    // 输出分块上限
static const int SUB_MARGIN = 8;        // 子渲染冗余像素（每边）
static int s_totalW, s_totalH;          // 总像素尺寸
static double s_gsdWorld;               // 世界坐标单位/像素
static double s_fullHalfW, s_fullHalfH; // 场景半宽半高
static osg::Vec3d s_sceneCenter;
static std::string s_saveBase;
static int s_tileSaved = 0;

// Level 1: 输出分块网格（每块最大 MAX_TILE_PX×MAX_TILE_PX）
static int s_outTilesX = 1, s_outTilesY = 1;
static int s_outRow = 0, s_outCol = 0;
static int s_curOutTileW = 0, s_curOutTileH = 0;
static std::vector<unsigned char> s_tileBuf; // 累积缓冲区

// Level 2: 子渲染网格（viewport 大小）
static int s_subCols = 1, s_subRows = 1;
static int s_subRow = 0, s_subCol = 0;

// 操纵器状态保存
static osg::Vec3d s_savedManipCenter;
static double s_savedManipDist;
static osg::Quat s_savedManipRot;

// 计算当前子渲染的 viewport 中心世界坐标（相对于 sceneCenter）
// ★ 重叠裁切策略：
//   步进 innerW = vpW - 2*M 像素（相邻子渲染重叠 2*M 像素）
//   每个子渲染覆盖 vpW*gsd 世界宽度，1:1 GSD 不变
//   相机放在 viewport 中心，对称投影 ±vpW*gsd/2
//   拷贝时裁掉四边 margin 只留中间 innerW×innerH
static void calcSubRenderBounds() {
  int innerW = G.vpW - 2 * SUB_MARGIN;
  int innerH = G.vpH - 2 * SUB_MARGIN;
  int outPixelLeft = s_outCol * MAX_TILE_PX;
  int outPixelTop = s_outRow * MAX_TILE_PX;
  // 有效区域起始全局像素
  int effPixelLeft = outPixelLeft + s_subCol * innerW;
  int effPixelTop = outPixelTop + s_subRow * innerH;
  // viewport 中心对应全局像素 = eff + innerW/2（margin 自动居中）
  double vpCenterPxX = effPixelLeft + innerW * 0.5;
  double vpCenterPxY = effPixelTop + innerH * 0.5;
  // 转为世界坐标偏移（相对于 sceneCenter）
  s_subCenterX = -s_fullHalfW + vpCenterPxX * s_gsdWorld;
  s_subCenterY = s_fullHalfH - vpCenterPxY * s_gsdWorld;
}

// 初始化当前输出分块
static void startOutputTile() {
  int outPixelLeft = s_outCol * MAX_TILE_PX;
  int outPixelTop = s_outRow * MAX_TILE_PX;
  s_curOutTileW = std::min(MAX_TILE_PX, s_totalW - outPixelLeft);
  s_curOutTileH = std::min(MAX_TILE_PX, s_totalH - outPixelTop);
  int innerW = G.vpW - 2 * SUB_MARGIN;
  int innerH = G.vpH - 2 * SUB_MARGIN;
  s_subCols = (int)ceil((double)s_curOutTileW / innerW);
  s_subRows = (int)ceil((double)s_curOutTileH / innerH);
  s_subRow = 0;
  s_subCol = 0;
  s_tileBuf.assign((size_t)s_curOutTileW * s_curOutTileH * 3, 0);
  calcSubRenderBounds();
  s_orthoFrame = 0;
  FLOG(
      "[ORTHO] startOutputTile out=(%d,%d) size=%dx%d subs=%dx%d inner=%dx%d\n",
      s_outRow, s_outCol, s_curOutTileW, s_curOutTileH, s_subCols, s_subRows,
      innerW, innerH);
}

// 回读状态机
static osg::ref_ptr<osg::Image> s_orthoCapture;
static bool s_orthoCaptureReady = false;

struct OrthoReadCB : public osg::Camera::DrawCallback {
  osg::Image *dst;
  int w, h;
  bool *readyFlag;
  OrthoReadCB(osg::Image *d, int ww, int hh, bool *flag)
      : dst(d), w(ww), h(hh), readyFlag(flag) {}
  void operator()(osg::RenderInfo &) const override {
    dst->readPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE);
    *readyFlag = true;
  }
};

// 在主循环 frame() 之前调用
static void orthoPreFrame() {
  if (!s_orthoActive)
    return;
  FLOG("[ORTHO] orthoPreFrame frame=%d out=(%d,%d) sub=(%d,%d)\n", s_orthoFrame,
       s_outRow, s_outCol, s_subRow, s_subCol);

  // 禁用操纵器
  if (G.viewer->getCameraManipulator() != nullptr)
    G.viewer->setCameraManipulator(nullptr, false);

  // ★ 相机放在当前子渲染中心，对称正交投影
  double camX = s_sceneCenter.x() + s_subCenterX;
  double camY = s_sceneCenter.y() + s_subCenterY;
  double halfProjW = G.vpW * s_gsdWorld * 0.5;
  double halfProjH = G.vpH * s_gsdWorld * 0.5;
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      -halfProjW, halfProjW, -halfProjH, halfProjH, s_near, s_far);

  // 设置 ViewMatrix —— 相机在子渲染中心，始终正北朝上
  if (s_orthoNadir) {
    // TDOM（真正射）：严格垂直俯视
    osg::Vec3d eye(camX, camY, s_sceneCenter.z() + s_far * 0.45);
    osg::Vec3d at(camX, camY, s_sceneCenter.z());
    G.viewer->getCamera()->setViewMatrixAsLookAt(eye, at, osg::Vec3d(0, 1, 0));
  } else {
    // DOM（微倾斜）：仅提取俯仰角，强制正北方向
    osg::Vec3d rawDir = s_savedManipRot * osg::Vec3d(0, 0, -1);
    double cosA = std::max(-1.0, std::min(1.0, -rawDir.z()));
    double tilt = acos(cosA); // 从垂直方向的倾角
    if (tilt > 1.047)
      tilt = 1.047; // 最大 60°
    double dist = s_far * 0.45;
    osg::Vec3d at(camX, camY, s_sceneCenter.z());
    // 从正南方微倾看向正北，保持正北朝上
    osg::Vec3d eye(camX, camY - sin(tilt) * dist,
                   s_sceneCenter.z() + cos(tilt) * dist);
    G.viewer->getCamera()->setViewMatrixAsLookAt(eye, at, osg::Vec3d(0, 0, 1));
  }

  s_orthoFrame++;

  // 当帧数足够且 Pager 空闲时，预设回读回调
  // 主循环的 viewer->frame() 会自然触发回读（不再嵌套调用 frame）
  if (s_orthoFrame >= s_orthoTotal && !s_orthoCapture.valid()) {
    bool ready = true;
    osgDB::DatabasePager *pager = G.viewer->getDatabasePager();
    if (pager && s_orthoFrame < 300) {
      int pending = pager->getFileRequestListSize();
      if (pending > 0) {
        ready = false;
        if (G.hStat && (s_orthoFrame % 10 == 0)) {
          int tileIdx = s_outRow * s_outTilesX + s_outCol + 1;
          int tileTotal = s_outTilesX * s_outTilesY;
          std::wstring lb = L"\u7b49\u5f85\u52a0\u8f7d [" +
                            std::to_wstring(tileIdx) + L"/" +
                            std::to_wstring(tileTotal) + L"] \u5269\u4f59" +
                            std::to_wstring(pending) + L"\u4e2a\u8bf7\u6c42...";
          SetWindowTextW(G.hStat, lb.c_str());
        }
      } else if (s_orthoFrame < s_orthoTotal + 10) {
        ready = false;
      }
    }
    if (ready) {
      int outW = G.vpW, outH = G.vpH;
      s_orthoCapture = new osg::Image();
      s_orthoCapture->allocateImage(outW, outH, 1, GL_RGB, GL_UNSIGNED_BYTE);
      s_orthoCaptureReady = false;
      G.viewer->getCamera()->setFinalDrawCallback(new OrthoReadCB(
          s_orthoCapture.get(), outW, outH, &s_orthoCaptureReady));
    }
  }

  if (G.hStat && (s_orthoFrame % 5 == 0) && !s_orthoCapture.valid()) {
    int tileIdx = s_outRow * s_outTilesX + s_outCol + 1;
    int tileTotal = s_outTilesX * s_outTilesY;
    std::wstring lb =
        s_orthoNadir ? L"\u6e32\u67d3DOM " : L"\u6e32\u67d3\u6b63\u4ea4 ";
    lb += L"[" + std::to_wstring(tileIdx) + L"/" + std::to_wstring(tileTotal) +
          L"] ";
    lb += std::to_wstring(s_orthoFrame) + L"/" + std::to_wstring(s_orthoTotal);
    SetWindowTextW(G.hStat, lb.c_str());
  }
}

// 推进到下一个子渲染，返回 true 表示还有子渲染需要做
static bool advanceSubRender() {
  s_subCol++;
  if (s_subCol >= s_subCols) {
    s_subCol = 0;
    s_subRow++;
  }
  if (s_subRow >= s_subRows)
    return false; // 当前输出分块完成
  calcSubRenderBounds();
  s_orthoFrame = 0;
  return true;
}

// 在主循环 frame() 之后调用 —— 不再包含嵌套的 viewer->frame()
static void orthoPostFrame() {
  if (!s_orthoActive)
    return;
  FLOG("[ORTHO] orthoPostFrame captureReady=%d\n", (int)s_orthoCaptureReady);

  // 等待回读回调完成（由主循环的 viewer->frame() 触发）
  if (!s_orthoCaptureReady)
    return;

  // 回读完成，移除回调
  G.viewer->getCamera()->setFinalDrawCallback(nullptr);

  // ── 将 viewport 像素拷贝到输出分块缓冲区（top-down 存储）──
  // 1:1 GSD，跳过边缘 SUB_MARGIN 像素，只取中间有效区域
  int vpW = G.vpW, vpH = G.vpH;
  int innerW = vpW - 2 * SUB_MARGIN;
  int innerH = vpH - 2 * SUB_MARGIN;
  const unsigned char *src = s_orthoCapture->data();
  int dstOffX = s_subCol * innerW;
  int dstOffY = s_subRow * innerH;
  for (int row = 0; row < innerH; row++) {
    // src row=0 是 GL 底部(南)，翻转为 top-down 并跳过底部 margin
    int srcRow = (vpH - 1 - SUB_MARGIN) - row;
    int dstY = dstOffY + row;
    if (dstY >= s_curOutTileH)
      break;
    int copyW = std::min(innerW, s_curOutTileW - dstOffX);
    if (copyW <= 0)
      break;
    memcpy(&s_tileBuf[((size_t)dstY * s_curOutTileW + dstOffX) * 3],
           &src[((size_t)srcRow * vpW + SUB_MARGIN) * 3], (size_t)copyW * 3);
  }
  FLOG("[ORTHO] copied viewport (%d,%d) inner=%dx%d to buf offset (%d,%d)\n",
       s_subCol, s_subRow, innerW, innerH, dstOffX, dstOffY);

  s_orthoCapture = nullptr;
  s_orthoCaptureReady = false;

  // ── 推进子渲染 ──
  if (advanceSubRender()) {
    FLOG("[ORTHO] advanceSubRender: more sub-renders for output tile\n");
    return;
  }

  // ── 当前输出分块的所有子渲染完成，写入 TIFF ──
  FLOG("[ORTHO] output tile (%d,%d) complete, writing TIFF %dx%d\n", s_outRow,
       s_outCol, s_curOutTileW, s_curOutTileH);

  std::string tilePath;
  if (s_outTilesX == 1 && s_outTilesY == 1) {
    tilePath = s_saveBase + ".tif";
  } else {
    tilePath = s_saveBase + "_R" + std::to_string(s_outRow) + "_C" +
               std::to_string(s_outCol) + ".tif";
  }

  bool ok = writeTiff(tilePath, s_tileBuf.data(), s_curOutTileW, s_curOutTileH);
  FLOG("[ORTHO] writeTiff result=%d path=%s\n", (int)ok, tilePath.c_str());
  if (ok) {
    auto stem = [](const std::string &p) {
      auto d = p.rfind('.');
      return d != std::string::npos ? p.substr(0, d) : p;
    };
    // TFW 坐标基于输出分块在全局坐标系中的位置
    int outPixelLeft = s_outCol * MAX_TILE_PX;
    int outPixelTop = s_outRow * MAX_TILE_PX;
    double tileWorldL =
        s_sceneCenter.x() - s_fullHalfW + outPixelLeft * s_gsdWorld;
    double tileWorldT =
        s_sceneCenter.y() + s_fullHalfH - outPixelTop * s_gsdWorld;
    writeTfw(stem(tilePath) + ".tfw", tileWorldL, tileWorldT, s_gsdWorld,
             s_gsdWorld);
    FLOG("[ORTHO] writeTfw done\n");
    if (G.metaValid && !G.metaSrs.empty())
      writePrj(stem(tilePath) + ".prj", G.metaSrs);
    FLOG("[ORTHO] writePrj done\n");
    s_tileSaved++;
  }

  // ── 推进到下一个输出分块 ──
  s_outCol++;
  if (s_outCol >= s_outTilesX) {
    s_outCol = 0;
    s_outRow++;
  }
  if (s_outRow < s_outTilesY) {
    FLOG("[ORTHO] advancing to next output tile (%d,%d)\n", s_outRow, s_outCol);
    startOutputTile();
    return;
  }

  // ── 所有输出分块完成 ──
  FLOG("[ORTHO] ALL output tiles done, restoring state\n");
  G.viewer->getCamera()->setProjectionMatrix(s_savedProj);
  FLOG("[ORTHO] restoring manipulator... G.manip=%p\n", (void *)G.manip);
  if (G.manip) {
    G.viewer->setCameraManipulator(G.manip, false);
    G.manip->setCenter(s_savedManipCenter);
    G.manip->setDistance(s_savedManipDist);
    G.manip->setRotation(s_savedManipRot);
  }
  FLOG("[ORTHO] restoring skyDome and HUD...\n");
  if (G.skyDome.valid())
    G.skyDome->setNodeMask(~0u);
  if (G.hudInfo.valid())
    G.hudInfo->setNodeMask(G.hudInfoOn ? ~0u : 0u);
  if (G.hudTips.valid())
    G.hudTips->setNodeMask(G.hudTipsOn ? ~0u : 0u);
  s_orthoActive = false;
  s_exporting = false;
  FLOG("[ORTHO] state flags reset\n");

  // 构建完成消息（延迟到主循环中安全显示）
  auto modeW = s_orthoNadir ? L"\u6b63\u5c04DOM" : L"\u6b63\u4ea4\u622a\u56fe";
  int totalTiles = s_outTilesX * s_outTilesY;
  if (G.hStat)
    SetWindowTextW(G.hStat,
                   (std::wstring(modeW) + L" \u5bfc\u51fa\u5b8c\u6210: " +
                    std::to_wstring(s_tileSaved) + L" \u4e2a\u5206\u5757")
                       .c_str());

  s_orthoDoneMsg =
      std::wstring(modeW) + L"\u5df2\u6210\u529f\u5bfc\u51fa\uff01\n\n";
  s_orthoDoneMsg += L"\u5730\u9762\u5206\u8fa8\u7387: " +
                    std::to_wstring(s_orthoGSD) + L" \u7c73/\u50cf\u7d20\n";
  s_orthoDoneMsg += L"\u603b\u50cf\u7d20: " + std::to_wstring(s_totalW) +
                    L" \u00d7 " + std::to_wstring(s_totalH) + L"\n";
  if (totalTiles > 1)
    s_orthoDoneMsg += L"\u5206\u5757: " + std::to_wstring(s_outTilesX) +
                      L" \u00d7 " + std::to_wstring(s_outTilesY) + L" = " +
                      std::to_wstring(totalTiles) + L" \u4e2a\n";
  s_orthoDoneMsg += L"\u6587\u4ef6: " + W(s_saveBase) + L"*.tif";
  FLOG("[ORTHO] export ALL DONE, tileSaved=%d\n", s_tileSaved);
  s_orthoJustDone = true;
}

static void doOrthoExport(bool nadir) {
  FLOG("[ORTHO] doOrthoExport nadir=%d\n", (int)nadir);
  if (s_exporting)
    return;
  if (!G.viewer || !G.sceneRoot) {
    MessageBoxW(G.hMain, L"\u8bf7\u5148\u6253\u5f00 OSGB \u6570\u636e\uff01",
                L"\u63d0\u793a", MB_OK | MB_ICONWARNING);
    return;
  }
  osg::BoundingSphere bs = G.sceneRoot->getBound();
  if (!bs.valid()) {
    MessageBoxW(G.hMain,
                L"\u573a\u666f\u65e0\u6709\u6548\u5305\u56f4\u76d2\uff01",
                L"\u9519\u8bef", MB_OK | MB_ICONERROR);
    return;
  }

  // \u663e\u793a GSD \u5bf9\u8bdd\u6846
  if (!showOrthoDlg())
    return;

  // \u8ba1\u7b97\u573a\u666f\u8303\u56f4
  double fovy, aspect, zNear, zFar;
  G.viewer->getCamera()->getProjectionMatrixAsPerspective(fovy, aspect, zNear,
                                                          zFar);
  osg::Vec3d center = G.manip->getCenter();
  double dist = G.manip->getDistance();
  double halfH = dist * tan(osg::DegreesToRadians(fovy * 0.5));
  double halfW = halfH * aspect;

  // GSD \u6362\u7b97\uff1a\u5224\u65ad\u662f\u5426 WGS84
  bool isWGS84 = G.metaValid && (G.metaSrs.find("4326") != std::string::npos ||
                                 G.metaSrs.find("WGS") != std::string::npos ||
                                 G.metaSrs.find("wgs") != std::string::npos);
  if (isWGS84) {
    double lat = G.metaLat;
    double mPerDeg = 111320.0 * cos(lat * 3.14159265358979 / 180.0);
    s_gsdWorld = s_orthoGSD / mPerDeg; // \u7c73 \u2192 \u5ea6
  } else {
    s_gsdWorld =
        s_orthoGSD; // \u6295\u5f71\u5750\u6807\uff0c\u5355\u4f4d\u5df2\u662f\u7c73
  }

  // \u8ba1\u7b97\u603b\u8f93\u51fa\u5c3a\u5bf8
  double sceneW = halfW * 2.0, sceneH = halfH * 2.0;
  s_totalW = (int)ceil(sceneW / s_gsdWorld);
  s_totalH = (int)ceil(sceneH / s_gsdWorld);

  // 计算输出分块网格（每块最大 MAX_TILE_PX×MAX_TILE_PX）
  s_outTilesX = (int)ceil((double)s_totalW / MAX_TILE_PX);
  s_outTilesY = (int)ceil((double)s_totalH / MAX_TILE_PX);

  // 确认导出信息
  std::wstring info = L"\u5730\u9762\u5206\u8fa8\u7387: " +
                      std::to_wstring(s_orthoGSD) + L" \u7c73/\u50cf\u7d20\n";
  if (isWGS84)
    info += L"WGS84 \u50cf\u7d20\u8ddd: " + std::to_wstring(s_gsdWorld) +
            L" \u5ea6\n";
  info += L"\u603b\u50cf\u7d20: " + std::to_wstring(s_totalW) + L" \u00d7 " +
          std::to_wstring(s_totalH) + L"\n";
  info += L"\u8f93\u51fa\u5206\u5757: " + std::to_wstring(s_outTilesX) +
          L" \u00d7 " + std::to_wstring(s_outTilesY);
  info += L" (\u6bcf\u5757\u6700\u5927 " + std::to_wstring(MAX_TILE_PX) +
          L"\u00d7" + std::to_wstring(MAX_TILE_PX) + L")\n\n";
  info += L"\u662f\u5426\u7ee7\u7eed\uff1f";
  if (MessageBoxW(G.hMain, info.c_str(), L"\u786e\u8ba4\u5bfc\u51fa",
                  MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
    return;

  // 选择保存路径
  std::string savePath = pickSaveTif();
  if (savePath.empty())
    return;
  // 提取不含扩展名的基础名
  auto dotPos = savePath.rfind('.');
  s_saveBase =
      (dotPos != std::string::npos) ? savePath.substr(0, dotPos) : savePath;

  // 初始化导出状态
  s_exporting = true;
  s_orthoActive = true;
  s_orthoNadir = nadir;

  // 隐藏天空穹顶和 HUD，避免蓝天遮挡地面 / 文字叠加到导出影像
  if (G.skyDome.valid())
    G.skyDome->setNodeMask(0);
  if (G.hudInfo.valid())
    G.hudInfo->setNodeMask(0);
  if (G.hudTips.valid())
    G.hudTips->setNodeMask(0);
  s_orthoFrame = 0;
  s_outRow = 0;
  s_outCol = 0;
  s_tileSaved = 0;

  s_savedProj = G.viewer->getCamera()->getProjectionMatrix();
  if (G.manip) {
    s_savedManipCenter = G.manip->getCenter();
    s_savedManipDist = G.manip->getDistance();
    s_savedManipRot = G.manip->getRotation();
  }

  double sceneH2 = bs.radius() * 4.0;
  s_sceneCenter = center;
  s_fullHalfW = halfW;
  s_fullHalfH = halfH;
  s_near = 0.1;
  s_far = sceneH2 * 2.0;

  // 初始化第一个输出分块
  startOutputTile();

  auto label = nadir ? L"\u6b63\u5728\u6e32\u67d3\u6b63\u5c04DOM..."
                     : L"\u6b63\u5728\u6e32\u67d3\u6b63\u4ea4\u622a\u56fe...";
  if (G.hStat)
    SetWindowTextW(G.hStat, label);
}

// \u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550
// \u5168\u666f\u8f93\u51fa
// \u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550

// ── 12K 全景常量 ──
static const int PANO_OUT_W = 12288;    // 等距矩形输出宽度
static const int PANO_OUT_H = 6144;     // 等距矩形输出高度
static const int PANO_FACE_SIZE = 3072; // 每面渲染尺寸
static const double PANO_FOV = 92.0;    // 立方体面FOV（>90°产生重叠）
// FOV缩放因子：92°时投影范围为 [-tan(46°), tan(46°)]
static const double PANO_FOV_SCALE = 1.03553; // tan(46°)

// CubeMap \u516d\u9762\u65b9\u5411\u5b9a\u4e49 (Z-up)
struct CubeFaceDir {
  osg::Vec3d fwd, right, up;
};
static const CubeFaceDir s_cubeDirs[6] = {
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},   // 0 Front  +Y
    {{0, -1, 0}, {-1, 0, 0}, {0, 0, 1}}, // 1 Back   -Y
    {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}},  // 2 Right  +X
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  // 3 Left   -X
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},  // 4 Top    +Z
    {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}},  // 5 Bottom -Z
};

static bool s_panoActive = false;
static int s_panoFace = 0; // 0..5
static int s_panoFrame = 0;
static int s_panoTotal = 30;
static double s_panoHeight = 50.0;
static double s_panoFar = 10000.0;
static int s_panoFaceSize = 0;
static osg::Vec3d s_panoEye;
static osg::Matrixd s_panoProjSaved;
static osg::ref_ptr<osg::Image> s_panoFaces[6];
static std::string s_panoSavePath;
static bool s_panoJustDone = false; // 主循环退出防护标志

// 回读状态机（与正射导出相同模式）
static osg::ref_ptr<osg::Image> s_panoCapture;
static bool s_panoCaptureReady = false;

// \u64cd\u7eb5\u5668\u72b6\u6001\u4fdd\u5b58\uff08\u5168\u666f\u7528\uff09
static osg::Vec3d s_panoSavedCenter;
static double s_panoSavedDist;
static osg::Quat s_panoSavedRot;

// \u4fdd\u5b58JPG\u6587\u4ef6\u5bf9\u8bdd\u6846
static std::string pickSaveJpg() {
  wchar_t buf[MAX_PATH] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = G.hMain;
  ofn.lpstrFilter = L"JPEG (*.jpg)\0*.jpg\0\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"jpg";
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrTitle = L"\u4fdd\u5b58\u5168\u666f\u56fe";
  if (GetSaveFileNameW(&ofn))
    return U(buf);
  return {};
}

// \u9ad8\u5ea6\u8f93\u5165\u5bf9\u8bdd\u6846
static INT_PTR CALLBACK PanoDlgProc(HWND hd, UINT msg, WPARAM wp, LPARAM) {
  switch (msg) {
  case WM_INITDIALOG: {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.1f", s_panoHeight);
    SetDlgItemTextW(hd, 101, buf);
    return TRUE;
  }
  case WM_COMMAND:
    if (LOWORD(wp) == IDOK) {
      wchar_t buf[64] = {};
      GetDlgItemTextW(hd, 101, buf, 64);
      s_panoHeight = _wtof(buf);
      if (s_panoHeight < 0.1)
        s_panoHeight = 0.1;
      EndDialog(hd, IDOK);
      return TRUE;
    }
    if (LOWORD(wp) == IDCANCEL) {
      EndDialog(hd, IDCANCEL);
      return TRUE;
    }
    break;
  case WM_CLOSE:
    EndDialog(hd, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}

static bool showPanoDlg() {
  alignas(4) BYTE buf[2048] = {};
  BYTE *p = buf;
  auto *dlg = (DLGTEMPLATE *)p;
  dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
  dlg->cdit = 4;
  dlg->cx = 200;
  dlg->cy = 60;
  p += sizeof(DLGTEMPLATE);
  *(WORD *)p = 0;
  p += 2;
  *(WORD *)p = 0;
  p += 2;
  const wchar_t *ttl = L"\u5168\u666f\u8f93\u51fa - \u89c6\u70b9\u9ad8\u5ea6";
  size_t tl = (wcslen(ttl) + 1) * 2;
  memcpy(p, ttl, tl);
  p += tl;

  auto addCtrl = [&](DWORD st, short x, short y, short cx, short cy, WORD id,
                     WORD cls, const wchar_t *txt) {
    p = (BYTE *)(((uintptr_t)p + 3) & ~3);
    auto *it = (DLGITEMTEMPLATE *)p;
    it->style = st | WS_CHILD | WS_VISIBLE;
    it->x = x;
    it->y = y;
    it->cx = cx;
    it->cy = cy;
    it->id = id;
    p += sizeof(DLGITEMTEMPLATE);
    *(WORD *)p = 0xFFFF;
    p += 2;
    *(WORD *)p = cls;
    p += 2;
    size_t n = (wcslen(txt) + 1) * 2;
    memcpy(p, txt, n);
    p += n;
    *(WORD *)p = 0;
    p += 2;
  };
  addCtrl(SS_LEFT, 10, 14, 65, 10, (WORD)-1, 0x0082,
          L"\u89c6\u70b9\u9ad8\u5ea6(\u7c73):");
  addCtrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 80, 12, 50, 14, 101, 0x0081,
          L"50");
  addCtrl(BS_DEFPUSHBUTTON | WS_TABSTOP, 35, 38, 55, 16, IDOK, 0x0080,
          L"\u786e\u5b9a\u5bfc\u51fa");
  addCtrl(WS_TABSTOP, 105, 38, 55, 16, IDCANCEL, 0x0080, L"\u53d6\u6d88");

  return DialogBoxIndirectParamW(G.hInst, (DLGTEMPLATE *)buf, G.hMain,
                                 PanoDlgProc, 0) == IDOK;
}

// CubeMap → 等距矩形投影（双线性插值 + FOV 重叠缩放）
static osg::ref_ptr<osg::Image> cubemapToEquirect(int outW, int outH) {
  int fs = s_panoFaceSize;
  osg::ref_ptr<osg::Image> eq = new osg::Image();
  eq->allocateImage(outW, outH, 1, GL_RGB, GL_UNSIGNED_BYTE);
  unsigned char *dst = eq->data();

  for (int py = 0; py < outH; py++) {
    double theta = ((double)py / (outH - 1)) * M_PI; // 0=zenith, PI=nadir
    double sinT = sin(theta), cosT = cos(theta);
    for (int px = 0; px < outW; px++) {
      double phi = ((double)px / outW) * 2.0 * M_PI - M_PI; // -PI..PI

      double dx = sinT * sin(phi);
      double dy = sinT * cos(phi);
      double dz = cosT;

      double ax = fabs(dx), ay = fabs(dy), az = fabs(dz);
      int face;
      if (ay >= ax && ay >= az)
        face = (dy > 0) ? 0 : 1;
      else if (ax >= az)
        face = (dx > 0) ? 2 : 3;
      else
        face = (dz > 0) ? 4 : 5;

      const auto &fd = s_cubeDirs[face];
      double fwdDot = dx * fd.fwd.x() + dy * fd.fwd.y() + dz * fd.fwd.z();
      double u =
          (dx * fd.right.x() + dy * fd.right.y() + dz * fd.right.z()) / fwdDot;
      double v = (dx * fd.up.x() + dy * fd.up.y() + dz * fd.up.z()) / fwdDot;

      // FOV 缩放：92°FOV 渲染时，投影范围为 [-PANO_FOV_SCALE, PANO_FOV_SCALE]
      // 将标准 [-1,1] 映射到实际像素坐标
      u = u / PANO_FOV_SCALE;
      v = v / PANO_FOV_SCALE;

      // [-1,1] → 浮点像素坐标 (osg::Image bottom-up, v=-1=row0=bottom)
      double ffx = (u + 1.0) * 0.5 * (fs - 1);
      double ffy = (v + 1.0) * 0.5 * (fs - 1);

      // ── 双线性插值 ──
      int x0 = std::clamp((int)floor(ffx), 0, fs - 2);
      int y0 = std::clamp((int)floor(ffy), 0, fs - 2);
      int x1 = x0 + 1, y1 = y0 + 1;
      double fx = ffx - x0, fy = ffy - y0;
      fx = std::clamp(fx, 0.0, 1.0);
      fy = std::clamp(fy, 0.0, 1.0);

      const unsigned char *p00 = s_panoFaces[face]->data() + (y0 * fs + x0) * 3;
      const unsigned char *p10 = s_panoFaces[face]->data() + (y0 * fs + x1) * 3;
      const unsigned char *p01 = s_panoFaces[face]->data() + (y1 * fs + x0) * 3;
      const unsigned char *p11 = s_panoFaces[face]->data() + (y1 * fs + x1) * 3;

      // 输出: py=0 是天顶 → osg::Image 里是最后一行
      unsigned char *d = dst + ((outH - 1 - py) * outW + px) * 3;
      for (int c = 0; c < 3; c++) {
        double top = p00[c] * (1 - fx) + p10[c] * fx;
        double bot = p01[c] * (1 - fx) + p11[c] * fx;
        double val = top * (1 - fy) + bot * fy;
        d[c] = (unsigned char)std::clamp((int)(val + 0.5), 0, 255);
      }
    }
  }
  return eq;
}

// \u5168\u666f pre/post frame
static void panoPreFrame() {
  if (!s_panoActive)
    return;

  // 设置方形视口，确保 1:1 CubeMap 渲染无畸变
  int fs = s_panoFaceSize;
  G.viewer->getCamera()->setViewport(0, 0, fs, fs);

  // 设置 92° FOV 透视投影（>90°产生重叠区域，消除拼接缝）
  G.viewer->getCamera()->setProjectionMatrixAsPerspective(PANO_FOV, 1.0, 0.1,
                                                          s_panoFar);

  // \u8bbe\u7f6e\u5f53\u524d\u9762\u7684\u89c6\u89d2\u65b9\u5411
  const auto &fd = s_cubeDirs[s_panoFace];
  osg::Vec3d target = s_panoEye + fd.fwd * 100.0;
  G.viewer->getCamera()->setViewMatrixAsLookAt(s_panoEye, target, fd.up);

  s_panoFrame++;
  if (G.hStat && (s_panoFrame % 5 == 0)) {
    static const wchar_t *faceNames[] = {L"Front(+Y)", L"Back(-Y)",
                                         L"Right(+X)", L"Left(-X)",
                                         L"Top(+Z)",   L"Bottom(-Z)"};
    std::wstring lb =
        L"\u5168\u666f\u6e32\u67d3 [" + std::to_wstring(s_panoFace + 1) +
        L"/6] " + faceNames[s_panoFace] + L" " + std::to_wstring(s_panoFrame);
    SetWindowTextW(G.hStat, lb.c_str());
  }
}

static void panoPostFrame() {
  if (!s_panoActive)
    return;

  // 自适应等待：至少 s_panoTotal 帧 + DatabasePager 空闲
  if (s_panoFrame < s_panoTotal && !s_panoCapture.valid())
    return;

  // ── 阶段1：帧数足够且Pager空闲时，注册回读回调 ──
  if (s_panoFrame >= s_panoTotal && !s_panoCapture.valid()) {
    bool ready = true;
    osgDB::DatabasePager *pager = G.viewer->getDatabasePager();
    if (pager && s_panoFrame < 300) {
      int pending = pager->getFileRequestListSize();
      if (pending > 0) {
        ready = false;
      } else if (s_panoFrame < s_panoTotal + 10) {
        ready = false;
      }
    }
    if (ready) {
      int fs = s_panoFaceSize;
      s_panoCapture = new osg::Image();
      s_panoCapture->allocateImage(fs, fs, 1, GL_RGB, GL_UNSIGNED_BYTE);
      s_panoCaptureReady = false;

      struct PanoReadCB : public osg::Camera::DrawCallback {
        osg::Image *dst;
        int sz;
        bool *readyFlag;
        PanoReadCB(osg::Image *d, int s, bool *flag)
            : dst(d), sz(s), readyFlag(flag) {}
        void operator()(osg::RenderInfo &) const override {
          dst->readPixels(0, 0, sz, sz, GL_RGB, GL_UNSIGNED_BYTE);
          *readyFlag = true;
        }
      };
      G.viewer->getCamera()->setFinalDrawCallback(
          new PanoReadCB(s_panoCapture.get(), fs, &s_panoCaptureReady));
    }
    return;
  }

  // ── 阶段2：等待回读完成 ──
  if (!s_panoCaptureReady)
    return;

  // 回读完成，移除回调
  G.viewer->getCamera()->setFinalDrawCallback(nullptr);

  // 保存当前面
  int fs = s_panoFaceSize;
  s_panoFaces[s_panoFace] = s_panoCapture;
  s_panoCapture = nullptr;
  s_panoCaptureReady = false;
  FLOG("[PANO] face %d captured (%dx%d)\n", s_panoFace, fs, fs);

  // 推进到下一面
  s_panoFace++;
  if (s_panoFace < 6) {
    s_panoFrame = 0;
    return;
  }

  // ── 全部 6 面完成 → 拼接 ──
  if (G.hStat)
    SetWindowTextW(G.hStat, L"正在拼接12K全景图...");

  int outW = PANO_OUT_W, outH = PANO_OUT_H;
  auto equirect = cubemapToEquirect(outW, outH);

  // 保存 JPG
  bool ok = osgDB::writeImageFile(*equirect, s_panoSavePath);

  // 恢复相机视口、投影和 HUD
  G.viewer->getCamera()->setViewport(0, 0, G.vpW, G.vpH);
  G.viewer->getCamera()->setProjectionMatrix(s_panoProjSaved);
  if (G.hudInfo.valid())
    G.hudInfo->setNodeMask(G.hudInfoOn ? ~0u : 0u);
  if (G.hudTips.valid())
    G.hudTips->setNodeMask(G.hudTipsOn ? ~0u : 0u);
  G.viewer->setCameraManipulator(G.manip, false);
  if (G.manip) {
    G.manip->setCenter(s_panoSavedCenter);
    G.manip->setDistance(s_panoSavedDist);
    G.manip->setRotation(s_panoSavedRot);
  }
  s_panoActive = false;

  // 释放面图像
  for (int i = 0; i < 6; i++)
    s_panoFaces[i] = nullptr;

  // 退出防护 (MessageBox 前)
  G.viewer->setDone(false);
  {
    MSG tmpMsg;
    while (PeekMessageW(&tmpMsg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
      ;
  }

  if (ok) {
    std::wstring msg = L"全景图已成功导出！\n\n";
    msg += L"分辨率: " + std::to_wstring(outW) + L" × " +
           std::to_wstring(outH) + L"\n";
    msg += L"视点高度: " + std::to_wstring((int)s_panoHeight) + L" 米\n";
    msg += L"面尺寸: " + std::to_wstring(fs) + L" × " + std::to_wstring(fs) +
           L"\n";
    msg += L"文件: " + W(s_panoSavePath);
    MessageBoxW(G.hMain, msg.c_str(), L"全景导出完成",
                MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(G.hMain,
                L"全景图保存失败！\n请检查 "
                L"OSG jpeg 插件是否可用。",
                L"错误", MB_OK | MB_ICONERROR);
  }

  // MessageBox 结束，真正结束导出
  s_exporting = false;
  if (G.hStat)
    SetWindowTextW(G.hStat, L"就绪");

  // 退出防护 (MessageBox 后)
  G.viewer->setDone(false);
  {
    MSG tmpMsg;
    while (PeekMessageW(&tmpMsg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
      ;
  }
  s_panoJustDone = true;
}

static void doPanoExport() {
  if (s_exporting)
    return;
  if (!G.viewer || !G.sceneRoot) {
    MessageBoxW(G.hMain, L"\u8bf7\u5148\u6253\u5f00 OSGB \u6570\u636e\uff01",
                L"\u63d0\u793a", MB_OK | MB_ICONWARNING);
    return;
  }
  osg::BoundingSphere bs = G.sceneRoot->getBound();
  if (!bs.valid()) {
    MessageBoxW(G.hMain,
                L"\u573a\u666f\u65e0\u6709\u6548\u5305\u56f4\u76d2\uff01",
                L"\u9519\u8bef", MB_OK | MB_ICONERROR);
    return;
  }

  if (!showPanoDlg())
    return;

  // \u9009\u62e9\u4fdd\u5b58\u8def\u5f84
  std::string savePath = pickSaveJpg();
  if (savePath.empty())
    return;

  // \u8ba1\u7b97\u89c6\u70b9\u4f4d\u7f6e\uff1a\u5f53\u524d\u5c4f\u5e55\u4e2d\u5fc3
  // + \u6307\u5b9a\u9ad8\u5ea6
  osg::Vec3d center = G.manip->getCenter();
  s_panoEye = osg::Vec3d(center.x(), center.y(), center.z() + s_panoHeight);
  s_panoFar = bs.radius() * 4.0;
  s_panoFaceSize =
      std::min(G.vpW, G.vpH); // 使用窗口短边（确保在默认 framebuffer 内渲染）
  s_panoSavePath = savePath;

  // \u4fdd\u5b58\u72b6\u6001
  s_panoProjSaved = G.viewer->getCamera()->getProjectionMatrix();
  if (G.manip) {
    s_panoSavedCenter = G.manip->getCenter();
    s_panoSavedDist = G.manip->getDistance();
    s_panoSavedRot = G.manip->getRotation();
  }

  // \u79fb\u9664\u64cd\u7eb5\u5668\uff0c\u76f4\u63a5\u63a7\u5236\u76f8\u673a
  G.viewer->setCameraManipulator(nullptr, false);

  // 不使用 FBO，直接在窗口 framebuffer 渲染（确保 readPixels 有效）

  // 隐藏 HUD，避免文字叠加到全景影像
  if (G.hudInfo.valid())
    G.hudInfo->setNodeMask(0);
  if (G.hudTips.valid())
    G.hudTips->setNodeMask(0);

  // \u786e\u8ba4
  int outW = PANO_OUT_W, outH = PANO_OUT_H;
  std::wstring info = L"\u89c6\u70b9\u9ad8\u5ea6: " +
                      std::to_wstring((int)s_panoHeight) + L" \u7c73\n";
  info += L"\u5168\u666f\u5206\u8fa8\u7387: " + std::to_wstring(outW) +
          L" \u00d7 " + std::to_wstring(outH) + L"\n";
  info += L"\u9700\u6e32\u67d3 6 "
          L"\u4e2a\u7acb\u65b9\u4f53\u9762\n\n\u662f\u5426\u7ee7\u7eed\uff1f";
  if (MessageBoxW(G.hMain, info.c_str(),
                  L"\u786e\u8ba4\u5168\u666f\u5bfc\u51fa",
                  MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
    // \u6062\u590d\u64cd\u7eb5\u5668
    G.viewer->setCameraManipulator(G.manip, false);
    return;
  }

  // \u542f\u52a8\u6e32\u67d3
  s_exporting = true;
  s_panoActive = true;
  s_panoFace = 0;
  s_panoFrame = 0;

  if (G.hStat)
    SetWindowTextW(G.hStat, L"\u6b63\u5728\u6e32\u67d3\u5168\u666f...");
}

// ════════════════ 程序化天空背景 ════════════════════════════════════════════

// 构建天空背景：独立 PRE_RENDER 相机 + 单位球天空渐变
// 关键：禁用深度写入，保证永远在场景后面，绝不会遮挡模型
static osg::Camera *buildSkyDome(float /*radius*/, osg::Vec3 /*center*/) {
  printf("[SKY] buildSkyDome (background camera mode)\n");

  // 天空颜色渐变 (cosP: 1=天顶, 0=水平线, -1=底部)
  auto skyColor = [](float t) -> osg::Vec4 {
    if (t > 0.6f)
      return osg::Vec4(0.22f, 0.42f, 0.82f, 1.0f);
    else if (t > 0.3f) {
      float f = (t - 0.3f) / 0.3f;
      return osg::Vec4(0.45f - 0.23f * f, 0.62f - 0.20f * f, 0.90f - 0.08f * f,
                       1.0f);
    } else if (t > 0.05f) {
      float f = (t - 0.05f) / 0.25f;
      return osg::Vec4(0.72f - 0.27f * f, 0.78f - 0.16f * f, 0.92f - 0.02f * f,
                       1.0f);
    } else if (t > -0.05f)
      return osg::Vec4(0.78f, 0.82f, 0.92f, 1.0f);
    else {
      float f = std::min(1.0f, (-t - 0.05f) / 0.45f);
      return osg::Vec4(0.55f - 0.15f * f, 0.58f - 0.15f * f, 0.65f - 0.10f * f,
                       1.0f);
    }
  };

  const int SEGS = 64, RINGS = 32;
  auto *geom = new osg::Geometry();
  auto *verts = new osg::Vec3Array();
  auto *colors = new osg::Vec4Array();
  for (int ring = 0; ring <= RINGS; ring++) {
    float phi = (float)M_PI * ring / RINGS;
    float sinP = sinf(phi), cosP = cosf(phi);
    for (int seg = 0; seg <= SEGS; seg++) {
      float theta = 2.0f * (float)M_PI * seg / SEGS;
      verts->push_back(osg::Vec3(sinP * cosf(theta), sinP * sinf(theta), cosP));
      colors->push_back(skyColor(cosP));
    }
  }
  for (int ring = 0; ring < RINGS; ring++) {
    auto *strip = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLE_STRIP);
    for (int seg = 0; seg <= SEGS; seg++) {
      strip->push_back(ring * (SEGS + 1) + seg);
      strip->push_back((ring + 1) * (SEGS + 1) + seg);
    }
    geom->addPrimitiveSet(strip);
  }
  geom->setVertexArray(verts);
  geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
  geom->setUseDisplayList(true);

  auto *skyGeode = new osg::Geode();
  skyGeode->addDrawable(geom);
  auto *ss = skyGeode->getOrCreateStateSet();
  ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
  ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
  ss->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false),
                           osg::StateAttribute::ON);

  // 独立背景相机：最先渲染，不写深度
  auto *skyCam = new osg::Camera();
  skyCam->setRenderOrder(osg::Camera::PRE_RENDER, -1);
  skyCam->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  skyCam->setClearColor(osg::Vec4(0.15f, 0.18f, 0.22f, 1.0f));
  skyCam->setAllowEventFocus(false);
  skyCam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
  skyCam->setProjectionMatrixAsPerspective(90.0, 1.0, 0.1, 10.0);
  skyCam->setViewMatrix(osg::Matrix::identity());
  skyCam->addChild(skyGeode);

  printf("[SKY] buildSkyDome done (background camera, %d verts)\n",
         (int)verts->size());
  return skyCam;
}

static void loadOsgb(const std::string &dir) {
  if (G.hStat)
    SetWindowTextW(G.hStat, (L"\u6b63\u5728\u626b\u63cf: " + W(dir)).c_str());
  readMeta(dir);
  auto roots = scanTiles(dir);
  if (roots.empty()) {
    MessageBoxW(G.hMain,
                L"\u672a\u627e\u5230 OSGB \u74e6\u7247\u6587\u4ef6\uff01",
                L"\u63d0\u793a", MB_OK | MB_ICONWARNING);
    return;
  }
  G.dataDir = dir;
  G.tileCount = 0;
  auto *tiles = new osg::Group();
  for (int i = 0; i < (int)roots.size(); i++) {
    auto n = osgDB::readRefNodeFile(roots[i]);
    if (n.valid()) {
      tiles->addChild(n.get());
      G.tileCount++;
    }
    if ((i + 1) % 10 == 0 && G.hStat)
      SetWindowTextW(G.hStat, (L"\u52a0\u8f7d\u4e2d " + std::to_wstring(i + 1) +
                               L"/" + std::to_wstring((int)roots.size()))
                                  .c_str());
  }
  // 光照
  auto *root = new osg::Group();
  auto *lt = new osg::Light();
  lt->setLightNum(0);
  lt->setPosition(osg::Vec4(1, 1, 2, 0));
  lt->setDiffuse(osg::Vec4(.85f, .85f, .8f, 1));
  lt->setAmbient(osg::Vec4(.35f, .35f, .38f, 1));
  lt->setSpecular(osg::Vec4(.15f, .15f, .15f, 1));
  auto *ls = new osg::LightSource();
  ls->setLight(lt);
  ls->setLocalStateSetModes(osg::StateAttribute::ON);
  root->addChild(ls);
  root->addChild(tiles);
  root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::ON);
  G.sceneRoot = root;
  if (G.viewer) {
    // 天空穹顶：半径 = 场景包围球 ×3，以场景中心为圆心
    osg::BoundingSphere bs = root->getBound();
    float skyR = bs.valid() ? (float)(bs.radius() * 3.0) : 5000.0f;
    osg::Vec3 skyCenter = bs.valid() ? bs.center() : osg::Vec3(0, 0, 0);
    G.skyDome = buildSkyDome(skyR, skyCenter);
    printf("[SKY] scene bs valid=%d center=(%.1f,%.1f,%.1f) r=%.1f skyR=%.1f\n",
           bs.valid(), bs.center().x(), bs.center().y(), bs.center().z(),
           bs.radius(), skyR);

    // 主相机正常清屏（颜色+深度）
    G.viewer->getCamera()->setClearMask(GL_COLOR_BUFFER_BIT |
                                        GL_DEPTH_BUFFER_BIT);

    auto *rootGrp = new osg::Group();
    rootGrp->addChild(G.skyDome.get());
    rootGrp->addChild(G.sceneRoot.get());
    if (G.hudInfo)
      rootGrp->addChild(G.hudInfo.get());
    if (G.hudTips)
      rootGrp->addChild(G.hudTips.get());
    G.viewer->setSceneData(rootGrp);
    if (auto *p = G.viewer->getDatabasePager())
      p->registerPagedLODs(G.viewer->getSceneData());
    G.viewer->getEventHandlers().clear();
    G.viewer->addEventHandler(new KeyHandler(G.sceneRoot.get()));
    G.viewer->addEventHandler(new osgViewer::StatsHandler());
    G.viewer->addEventHandler(new osgGA::StateSetManipulator(
        G.viewer->getCamera()->getOrCreateStateSet()));

    // 手动设置 home 位置（基于场景数据，不受天空球影响）
    if (G.manip && bs.valid()) {
      G.manip->setAutoComputeHomePosition(false);
      osg::Vec3d center = bs.center();
      double dist = bs.radius() * 2.5;
      osg::Vec3d eye = center + osg::Vec3d(0, -dist, dist * 0.4);
      G.manip->setHomePosition(eye, center, osg::Vec3d(0, 0, 1));
      G.manip->home(0);
      printf("[SKY] home set: eye=(%.1f,%.1f,%.1f) center=(%.1f,%.1f,%.1f)\n",
             eye.x(), eye.y(), eye.z(), center.x(), center.y(), center.z());
    } else if (G.manip) {
      G.manip->setAutoComputeHomePosition(true);
      G.manip->home(0);
    }
  }
  if (G.hSide)
    InvalidateRect(G.hSide, nullptr, TRUE);
  if (G.hStat)
    SetWindowTextW(G.hStat,
                   (L"\u5df2\u52a0\u8f7d " + std::to_wstring(G.tileCount) +
                    L" \u5757\u74e6\u7247\uff0c\u76ee\u5f55: " + W(dir))
                       .c_str());
}

// ════════════════ 打开目录对话框 ══════════════════════════════════════════
static std::string pickFolder() {
  std::string result;
  // 用旧式 SHBrowseForFolder（没有 shobjidl.h 依赖）
  BROWSEINFOW bi = {};
  bi.hwndOwner = G.hMain;
  bi.lpszTitle = L"\u9009\u62e9 OSGB \u6570\u636e\u76ee\u5f55";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (pidl) {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, buf))
      result = U(buf);
    CoTaskMemFree(pidl);
  }
  return result;
}

// ════════════════ 工具栏绘制 ══════════════════════════════════════════════
static RECT btnRC(int i) {
  RECT r;
  r.left = BTN_GAP + i * (BTN_W + BTN_GAP);
  r.top = BTN_GAP;
  r.right = r.left + BTN_W;
  r.bottom = TOOLBAR_H - BTN_GAP;
  return r;
}
static int hitBtn(int mx) {
  for (int i = 0; i < (int)G.btns.size(); i++) {
    RECT r = btnRC(i);
    if (mx >= r.left && mx < r.right)
      return i;
  }
  return -1;
}
static LRESULT CALLBACK ToolbarProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hw, &ps);
    RECT rc;
    GetClientRect(hw, &rc);
    FillRect(dc, &rc, hBrTool);
    // 底部线
    HPEN pen = CreatePen(PS_SOLID, 1, C_BORDER);
    auto op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, 0, rc.bottom - 1, nullptr);
    LineTo(dc, rc.right, rc.bottom - 1);
    SelectObject(dc, op);
    DeleteObject(pen);
    // 按钮
    HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < (int)G.btns.size(); i++) {
      auto &b = G.btns[i];
      RECT br = btnRC(i);
      bool on = (b.toggle && *b.toggle), hov = (G.btnHover == i);
      HBRUSH hbr = CreateSolidBrush(on ? C_ON : hov ? C_HOVER : C_TOOL);
      FillRect(dc, &br, hbr);
      DeleteObject(hbr);
      HPEN brp = CreatePen(PS_SOLID, 1,
                           on    ? C_ACCENT
                           : hov ? C_BORDER
                                 : C_TOOL);
      auto ob = (HPEN)SelectObject(dc, brp);
      auto obr = (HBRUSH)SelectObject(dc, (HBRUSH)GetStockObject(NULL_BRUSH));
      Rectangle(dc, br.left, br.top, br.right, br.bottom);
      SelectObject(dc, ob);
      SelectObject(dc, obr);
      DeleteObject(brp);
      SetTextColor(dc, on ? RGB(255, 255, 255) : C_TXT);
      DrawTextW(dc, b.label, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    DeleteObject(font);
    EndPaint(hw, &ps);
    return 0;
  }
  if (msg == WM_MOUSEMOVE) {
    int prev = G.btnHover;
    G.btnHover = hitBtn(GET_X_LPARAM(lp));
    if (prev != G.btnHover)
      InvalidateRect(hw, nullptr, FALSE);
    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hw, 0};
    TrackMouseEvent(&tme);
    return 0;
  }
  if (msg == WM_MOUSELEAVE) {
    G.btnHover = -1;
    InvalidateRect(hw, nullptr, FALSE);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    int idx = hitBtn(GET_X_LPARAM(lp));
    if (idx >= 0)
      PostMessageW(G.hMain, WM_COMMAND, G.btns[idx].id, 0);
    return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ════════════════ 侧边栏绘制 ══════════════════════════════════════════════
static LRESULT CALLBACK SidebarProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hw, &ps);
    RECT rc;
    GetClientRect(hw, &rc);
    FillRect(dc, &rc, hBrSide);
    HPEN pen = CreatePen(PS_SOLID, 1, C_BORDER);
    auto op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, rc.right - 1, 0, nullptr);
    LineTo(dc, rc.right - 1, rc.bottom);
    SelectObject(dc, op);
    DeleteObject(pen);

    HFONT bold = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0,
                             0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT norm = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT sml = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    SetBkMode(dc, TRANSPARENT);

    auto hline = [&](int y) {
      HPEN p = CreatePen(PS_SOLID, 1, C_BORDER);
      auto o = (HPEN)SelectObject(dc, p);
      MoveToEx(dc, 8, y, nullptr);
      LineTo(dc, rc.right - 8, y);
      SelectObject(dc, o);
      DeleteObject(p);
    };
    auto dtext = [&](const wchar_t *s, int x, int y, HFONT f, COLORREF c) {
      SelectObject(dc, f);
      SetTextColor(dc, c);
      RECT tr = {x, y, rc.right - 4, y + 20};
      DrawTextW(dc, s, -1, &tr,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    auto fillHdr = [&](int y, int h) {
      HBRUSH b = CreateSolidBrush(C_HDR);
      RECT r = {0, y, rc.right - 1, y + h};
      FillRect(dc, &r, b);
      DeleteObject(b);
    };

    int y = 0;
    fillHdr(y, 26);
    dtext(L"  \u56fe  \u5c42", 4, y + 5, bold, C_TXT);
    y += 28;
    dtext(L"\u25be \u4e09\u7ef4\u6a21\u578b", 10, y, bold, C_TXT);
    y += 22;
    if (!G.dataDir.empty()) {
      dtext((L"    \u25a0 OSGB (" + std::to_wstring(G.tileCount) + L" \u5757)")
                .c_str(),
            0, y, norm, C_ACCENT);
      y += 20;
      dtext((L"      " + W(fs::path(G.dataDir).filename().string())).c_str(), 0,
            y, sml, C_DIM);
      y += 18;
    } else {
      dtext(L"    \uff08\u672a\u6253\u5f00\u6570\u636e\uff09", 0, y, sml,
            C_DIM);
      y += 18;
    }
    y += 4;
    hline(y);
    y += 8;

    fillHdr(y, 22);
    dtext(L"  \u5143\u6570\u636e", 4, y + 4, bold, C_TXT);
    y += 26;
    if (G.metaValid) {
      std::wostringstream ss;
      ss << std::fixed;
      dtext(L"  \u7ecf\u5ea6:", 6, y, sml, C_DIM);
      y += 16;
      ss << std::setprecision(6) << L"  " << G.metaLon << L"\u00b0";
      dtext(ss.str().c_str(), 0, y, sml, C_TXT);
      y += 16;
      ss.str({});
      dtext(L"  \u7eac\u5ea6:", 6, y, sml, C_DIM);
      y += 16;
      ss << std::setprecision(6) << L"  " << G.metaLat << L"\u00b0";
      dtext(ss.str().c_str(), 0, y, sml, C_TXT);
      y += 16;
      ss.str({});
      dtext(L"  \u9ad8\u7a0b:", 6, y, sml, C_DIM);
      y += 16;
      ss << std::setprecision(1) << L"  " << G.metaH << L"m";
      dtext(ss.str().c_str(), 0, y, sml, C_TXT);
      y += 16;
      if (!G.metaSrs.empty()) {
        dtext(L"  SRS:", 6, y, sml, C_DIM);
        y += 16;
        dtext((L"  " + W(G.metaSrs)).c_str(), 0, y, sml, C_DIM);
        y += 16;
      }
    } else {
      dtext(L"  \uff08\u672a\u627e\u5230 metadata.xml\uff09", 6, y, sml, C_DIM);
      y += 18;
    }

    y += 4;
    hline(y);
    y += 8;
    fillHdr(y, 22);
    dtext(L"  \u663e\u793a", 4, y + 4, bold, C_TXT);
    y += 26;
    auto chk = [&](const wchar_t *lbl, bool on) {
      dtext((std::wstring(on ? L"  \u2611 " : L"  \u2610 ") + lbl).c_str(), 0,
            y, norm, on ? C_TXT : C_DIM);
      y += 20;
    };
    chk(L"\u4fe1\u606f HUD", G.hudInfoOn);
    chk(L"\u64cd\u4f5c\u63d0\u793a", G.hudTipsOn);
    chk(L"\u7ebf\u6846\u6a21\u5f0f", G.wireOn);
    chk(L"\u706f\u5149", G.lightOn);

    DeleteObject(bold);
    DeleteObject(norm);
    DeleteObject(sml);
    EndPaint(hw, &ps);
    return 0;
  }
  if (msg == WM_LBUTTONDOWN) {
    // 显示区域点击切换（大约从 y=220 开始，每行 20px）
    int y = GET_Y_LPARAM(lp);
    // 粗估：图层28+22+38+8+26+16*6+8+26=约232
    int base = 232;
    int row = (y - base) / 20;
    bool changed = false;
    if (row == 0) {
      G.hudInfoOn = !G.hudInfoOn;
      if (G.hudInfo)
        G.hudInfo->setNodeMask(G.hudInfoOn ? ~0u : 0u);
      changed = true;
    } else if (row == 1) {
      G.hudTipsOn = !G.hudTipsOn;
      if (G.hudTips)
        G.hudTips->setNodeMask(G.hudTipsOn ? ~0u : 0u);
      changed = true;
    } else if (row == 2) {
      G.wireOn = !G.wireOn;
      if (G.sceneRoot) {
        auto *pm = new osg::PolygonMode();
        pm->setMode(osg::PolygonMode::FRONT_AND_BACK,
                    G.wireOn ? osg::PolygonMode::LINE : osg::PolygonMode::FILL);
        G.sceneRoot->getOrCreateStateSet()->setAttribute(pm);
      }
      changed = true;
    } else if (row == 3) {
      G.lightOn = !G.lightOn;
      if (G.sceneRoot)
        G.sceneRoot->getOrCreateStateSet()->setMode(
            GL_LIGHTING,
            G.lightOn ? osg::StateAttribute::ON : osg::StateAttribute::OFF);
      changed = true;
    }
    if (changed) {
      InvalidateRect(hw, nullptr, FALSE);
      if (G.hTool)
        InvalidateRect(G.hTool, nullptr, FALSE);
    }
    return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ════════════════ 主窗口 WndProc ══════════════════════════════════════════
static LRESULT CALLBACK MainProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hw, 20, &dark, sizeof(dark));
    return 0;
  }
  case WM_SIZE: {
    if (wp == SIZE_MINIMIZED)
      break;
    int W = LOWORD(lp), H = HIWORD(lp);
    SetWindowPos(G.hTool, nullptr, 0, 0, W, TOOLBAR_H, SWP_NOZORDER);
    SetWindowPos(G.hSide, nullptr, 0, TOOLBAR_H, SIDEBAR_W,
                 H - TOOLBAR_H - SB_H, SWP_NOZORDER);
    G.vpW = W - SIDEBAR_W;
    G.vpH = H - TOOLBAR_H - SB_H;
    SetWindowPos(G.hVP, nullptr, SIDEBAR_W, TOOLBAR_H, G.vpW, G.vpH,
                 SWP_NOZORDER);
    // 同步 OSG 窗口大小
    if (G.hOsgWin)
      SetWindowPos(G.hOsgWin, nullptr, 0, 0, G.vpW, G.vpH, SWP_NOZORDER);
    SendMessageW(G.hStat, WM_SIZE, wp, lp);
    InvalidateRect(G.hTool, nullptr, FALSE);
    break;
  }
  case WM_ERASEBKGND: {
    RECT rc;
    GetClientRect(hw, &rc);
    FillRect((HDC)wp, &rc, hBrBg);
    return 1;
  }
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case ID_FILE_OPEN: {
      std::string d = pickFolder();
      if (!d.empty())
        loadOsgb(d);
      break;
    }
    case ID_FILE_EXIT:
      FLOG("[EXIT] Menu File->Exit, setting s_userWantsQuit\n");
      s_userWantsQuit = true;
      break;
    case ID_VIEW_HOME:
      if (G.manip)
        G.manip->home(0);
      break;
    case ID_VIEW_WIRE:
      G.wireOn = !G.wireOn;
      if (G.sceneRoot) {
        auto *pm = new osg::PolygonMode();
        pm->setMode(osg::PolygonMode::FRONT_AND_BACK,
                    G.wireOn ? osg::PolygonMode::LINE : osg::PolygonMode::FILL);
        G.sceneRoot->getOrCreateStateSet()->setAttribute(pm);
      }
      CheckMenuItem(G.hMenu, ID_VIEW_WIRE,
                    MF_BYCOMMAND | (G.wireOn ? MF_CHECKED : MF_UNCHECKED));
      InvalidateRect(G.hTool, nullptr, FALSE);
      InvalidateRect(G.hSide, nullptr, FALSE);
      break;
    case ID_VIEW_LIGHT:
      G.lightOn = !G.lightOn;
      if (G.sceneRoot)
        G.sceneRoot->getOrCreateStateSet()->setMode(
            GL_LIGHTING,
            G.lightOn ? osg::StateAttribute::ON : osg::StateAttribute::OFF);
      CheckMenuItem(G.hMenu, ID_VIEW_LIGHT,
                    MF_BYCOMMAND | (G.lightOn ? MF_CHECKED : MF_UNCHECKED));
      InvalidateRect(G.hTool, nullptr, FALSE);
      InvalidateRect(G.hSide, nullptr, FALSE);
      break;
    case ID_SHOW_HUD:
      G.hudInfoOn = !G.hudInfoOn;
      if (G.hudInfo)
        G.hudInfo->setNodeMask(G.hudInfoOn ? ~0u : 0u);
      CheckMenuItem(G.hMenu, ID_SHOW_HUD,
                    MF_BYCOMMAND | (G.hudInfoOn ? MF_CHECKED : MF_UNCHECKED));
      InvalidateRect(G.hTool, nullptr, FALSE);
      InvalidateRect(G.hSide, nullptr, FALSE);
      break;
    case ID_SHOW_TIPS:
      G.hudTipsOn = !G.hudTipsOn;
      if (G.hudTips)
        G.hudTips->setNodeMask(G.hudTipsOn ? ~0u : 0u);
      CheckMenuItem(G.hMenu, ID_SHOW_TIPS,
                    MF_BYCOMMAND | (G.hudTipsOn ? MF_CHECKED : MF_UNCHECKED));
      InvalidateRect(G.hTool, nullptr, FALSE);
      InvalidateRect(G.hSide, nullptr, FALSE);
      break;
    case ID_HELP_ABOUT:
      MessageBoxW(hw,
                  L"OSGB \u4e09\u7ef4\u6d4f\u89c8\u5668 v3.1\n\n\u57fa\u4e8e "
                  L"OpenSceneGraph\nPagedLOD "
                  L"\u6d41\u5f0f\u52a0\u8f7d\n\n\u5feb\u6377\u952e: "
                  L"F=\u590d\u4f4d  H=HUD  T=\u63d0\u793a  W=\u7ebf\u6846  "
                  L"L=\u706f\u5149  ESC=\u9000\u51fa",
                  L"\u5173\u4e8e", MB_OK | MB_ICONINFORMATION);
      break;
    case ID_ORTHO_EXPORT:
      doOrthoExport(true); // 正射/TDOM = 严格垂直俑视(nadir)
      break;
    case ID_ORTHO_DOM:
      doOrthoExport(false); // DOM = 微倾斜（可见建筑侧面）
      break;
    case ID_PANO_EXPORT:
      doPanoExport();
      break;
    }
    return 0;
  case WM_CLOSE:
    if (s_exporting) {
      FLOG("[EXIT] WM_CLOSE during export — IGNORED\n");
      return 0;
    }
    FLOG("[EXIT] WM_CLOSE received, setting s_userWantsQuit\n");
    s_userWantsQuit = true;
    return 0;
  case WM_DESTROY:
    FLOG("[EXIT] WM_DESTROY received, setting s_userWantsQuit\n");
    s_userWantsQuit = true;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ════════════════ 建立菜单 ════════════════════════════════════════════════
static HMENU buildMenu() {
  HMENU bar = CreateMenu();
  HMENU mF = CreatePopupMenu();
  AppendMenuW(mF, MF_STRING, ID_FILE_OPEN,
              L"\u6253\u5f00\u76ee\u5f55(&O)...\tCtrl+O");
  AppendMenuW(mF, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(mF, MF_STRING, ID_FILE_EXIT, L"\u9000\u51fa(&X)");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mF, L"\u5f00\u59cb(&F)");
  HMENU mS = CreatePopupMenu();
  AppendMenuW(mS, MF_STRING | MF_CHECKED, ID_SHOW_HUD, L"\u4fe1\u606f HUD(&H)");
  AppendMenuW(mS, MF_STRING | MF_CHECKED, ID_SHOW_TIPS,
              L"\u64cd\u4f5c\u63d0\u793a(&T)");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mS, L"\u663e\u793a(&V)");
  HMENU mV = CreatePopupMenu();
  AppendMenuW(mV, MF_STRING, ID_VIEW_HOME, L"\u98de\u5230\u5168\u5c40(&F)\tF");
  AppendMenuW(mV, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(mV, MF_STRING, ID_VIEW_WIRE, L"\u7ebf\u6846\u6a21\u5f0f(&W)\tW");
  AppendMenuW(mV, MF_STRING, ID_VIEW_LIGHT, L"\u5149\u7167\u5f00\u5173(&L)\tL");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mV, L"\u89c6\u56fe(&R)");
  HMENU mT = CreatePopupMenu();
  AppendMenuW(mT, MF_STRING, ID_ORTHO_EXPORT,
              L"\u6b63\u5c04\u8f93\u51fa(&O)...\tCtrl+P");
  AppendMenuW(mT, MF_STRING, ID_ORTHO_DOM, L"DOM\u8f93\u51fa(&D)...");
  AppendMenuW(mT, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(mT, MF_STRING, ID_PANO_EXPORT,
              L"\u5168\u666f\u8f93\u51fa(&P)...");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mT, L"\u5de5\u5177(&T)");
  HMENU mH = CreatePopupMenu();
  AppendMenuW(mH, MF_STRING, ID_HELP_ABOUT, L"\u5173\u4e8e(&A)...");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mH, L"\u5e2e\u52a9(&H)");
  return bar;
}

// ════════════════ main ════════════════════════════════════════════════════
int main(int argc, char **argv) {
  // 附加控制台（用于日志输出）
  AllocConsole();
  FILE *f = nullptr;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONOUT$", "w", stderr);
  FLOG("[MAIN] Program started\n");
  SetUnhandledExceptionFilter(CrashHandler);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icce = {sizeof(icce),
                               ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icce);
  G.hInst = GetModuleHandle(nullptr);

  hBrBg = CreateSolidBrush(C_BG);
  hBrTool = CreateSolidBrush(C_TOOL);
  hBrSide = CreateSolidBrush(C_SIDE);

  G.btns = {
      {ID_FILE_OPEN, L"\U0001f4c2 \u6253\u5f00", nullptr},
      {ID_VIEW_HOME, L"\u2302 \u590d\u4f4d", nullptr},
      {ID_VIEW_WIRE, L"\u2b21 \u7ebf\u6846", &G.wireOn},
      {ID_VIEW_LIGHT, L"\u2600 \u706f\u5149", &G.lightOn},
      {ID_SHOW_HUD, L"\u25a4 HUD", &G.hudInfoOn},
      {ID_SHOW_TIPS, L"? \u63d0\u793a", &G.hudTipsOn},
      {ID_ORTHO_EXPORT, L"\U0001f4f7 \u6b63\u5c04", nullptr},
      {ID_ORTHO_DOM, L"\U0001f5fa DOM", nullptr},
      {ID_PANO_EXPORT, L"\U0001f310 \u5168\u666f", nullptr},
  };

  // 注册窗口类
  auto reg = [&](const wchar_t *cls, WNDPROC proc, HBRUSH br) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.hInstance = G.hInst;
    wc.lpszClassName = cls;
    wc.lpfnWndProc = proc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = br;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);
  };
  reg(L"OsgbMain", MainProc, hBrBg);
  reg(L"OsgbTool", ToolbarProc, hBrTool);
  reg(L"OsgbSide", SidebarProc, hBrSide);
  {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.hInstance = G.hInst;
    wc.lpszClassName = L"OsgbVP";
    wc.lpfnWndProc = DefWindowProcW;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.style = CS_OWNDC;
    RegisterClassExW(&wc);
  }

  G.hMenu = buildMenu();
  const int IW = 1560, IH = 920;
  G.hMain = CreateWindowExW(
      0, L"OsgbMain", L"OSGB \u4e09\u7ef4\u6d4f\u89c8\u5668 v3.1",
      WS_OVERLAPPEDWINDOW, 80, 50, IW, IH, nullptr, G.hMenu, G.hInst, nullptr);

  G.hTool =
      CreateWindowExW(0, L"OsgbTool", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, IW,
                      TOOLBAR_H, G.hMain, nullptr, G.hInst, nullptr);
  G.hSide = CreateWindowExW(0, L"OsgbSide", nullptr, WS_CHILD | WS_VISIBLE, 0,
                            TOOLBAR_H, SIDEBAR_W, IH - TOOLBAR_H - SB_H,
                            G.hMain, nullptr, G.hInst, nullptr);
  G.hStat =
      CreateWindowExW(0, STATUSCLASSNAME, L"\u5c31\u7eea",
                      WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                      G.hMain, (HMENU)(INT_PTR)IDC_STATUS, G.hInst, nullptr);

  G.vpW = IW - SIDEBAR_W;
  G.vpH = IH - TOOLBAR_H - SB_H;
  G.hVP = CreateWindowExW(
      0, L"OsgbVP", nullptr,
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, SIDEBAR_W,
      TOOLBAR_H, G.vpW, G.vpH, G.hMain, nullptr, G.hInst, nullptr);

  ShowWindow(G.hMain, SW_SHOW);
  UpdateWindow(G.hMain);

  // ── OSG 初始化 ───────────────────────────────────────────────────────
  osgViewer::Viewer *viewer = new osgViewer::Viewer();
  // 创建一个独立的 OSG 窗口，然后通过 SetParent 嵌入到 hVP
  viewer->setUpViewInWindow(0, 0, G.vpW, G.vpH);
  viewer->getCamera()->setClearColor(
      osg::Vec4f(0.15f, 0.18f, 0.22f, 1.f)); // 深灰蓝背景
  viewer->setThreadingModel(osgViewer::Viewer::SingleThreaded);
  G.viewer = viewer;

  auto *manip = new osgGA::TrackballManipulator();
  manip->setAllowThrow(false);
  manip->setAutoComputeHomePosition(true);
  viewer->setCameraManipulator(manip);
  G.manip = manip;

  G.hudInfo = buildInfoHud(G.vpW, G.vpH);
  G.hudTips = buildTipsHud(G.vpW, G.vpH);
  auto *rootGrp = new osg::Group();
  rootGrp->addChild(G.hudInfo.get());
  rootGrp->addChild(G.hudTips.get());
  viewer->setSceneData(rootGrp);
  viewer->addEventHandler(new osgViewer::StatsHandler());
  viewer->addEventHandler(new osgGA::StateSetManipulator(
      viewer->getCamera()->getOrCreateStateSet()));

  osg::ref_ptr<osgDB::DatabasePager> pager = new osgDB::DatabasePager();
  pager->setUpThreads(4, 1);
  pager->setTargetMaximumNumberOfPageLOD(500);
  viewer->setDatabasePager(pager.get());
  viewer->realize();

  // 获取 OSG 创建的 HWND，嵌入到 hVP
  {
    osgViewer::Viewer::Windows wins;
    viewer->getWindows(wins);
    if (!wins.empty()) {
      auto *gw = dynamic_cast<osgViewer::GraphicsWindowWin32 *>(wins[0]);
      if (gw) {
        G.hOsgWin = gw->getHWND();
        // 移除 OSG 窗口的边框和标题栏，改为子窗口
        LONG_PTR style = GetWindowLongPtrW(G.hOsgWin, GWL_STYLE);
        style =
            (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU)) |
            WS_CHILD;
        SetWindowLongPtrW(G.hOsgWin, GWL_STYLE, style);
        SetParent(G.hOsgWin, G.hVP);
        SetWindowPos(G.hOsgWin, nullptr, 0, 0, G.vpW, G.vpH,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
      }
    }
  }

  // 处理命令行参数
  if (argc > 1) {
    std::string arg = argv[1];
    if (fs::is_directory(arg) || fs::is_regular_file(arg))
      loadOsgb(arg);
  }

  // ── 消息/渲染主循环（核武器方案：仅 s_userWantsQuit 能终止循环）────
  MSG winMsg;
  FLOG("[MAIN] Entering main loop\n");
  int frameCount = 0;
  while (!s_userWantsQuit) {
    while (PeekMessageW(&winMsg, nullptr, 0, 0, PM_REMOVE)) {
      if (winMsg.message == WM_QUIT) {
        FLOG("[MAIN] WM_QUIT received — SWALLOWED (frame %d)\n", frameCount);
        continue;
      }
      TranslateMessage(&winMsg);
      DispatchMessageW(&winMsg);
    }
    // ★ 每帧开头：无条件重置 viewer->done()，
    //   OSG 内部任何机制（事件遍历、CLOSE 事件等）设的 done=true 都被碾压
    if (viewer->done()) {
      FLOG("[GUARD] viewer->done() was true at frame %d — FORCE RESET!\n",
           frameCount);
      printf("[GUARD] viewer->done() was true — FORCE RESET!\n");
      viewer->setDone(false);
    }
    if (!IsIconic(G.hMain)) {
      updateInfoHud();
      // 同步天空背景相机旋转（只跟踪主相机旋转方向，不跟踪平移）
      if (G.skyDome.valid() && !s_orthoActive) {
        auto *skyCam = dynamic_cast<osg::Camera *>(G.skyDome.get());
        if (skyCam) {
          osg::Matrixd vm = viewer->getCamera()->getViewMatrix();
          // 提取纯旋转（去掉平移分量），让天空始终跟随视角旋转
          vm.setTrans(0, 0, 0);
          skyCam->setViewMatrix(vm);
        }
      }
      orthoPreFrame();
      panoPreFrame();
      viewer->frame(); // 直接调用 viewer->frame()
      orthoPostFrame();
      panoPostFrame();
      // ★ frame() 后再次碾压，防止 frame() 内部触发的 done=true
      if (viewer->done()) {
        FLOG("[GUARD] viewer->done() set during frame() at frame %d — FORCE "
             "RESET!\n",
             frameCount);
        printf("[GUARD] viewer->done() set during frame() — FORCE RESET!\n");
        viewer->setDone(false);
      }
      frameCount++;
      // 导出完成后的安全处理：在主循环中显示 MessageBox
      if (s_orthoJustDone) {
        s_orthoJustDone = false;
        if (!s_orthoDoneMsg.empty()) {
          s_exporting = true; // 保护 MessageBox 期间不被关闭
          MessageBoxW(G.hMain, s_orthoDoneMsg.c_str(),
                      L"\u5bfc\u51fa\u5b8c\u6210", MB_OK | MB_ICONINFORMATION);
          s_exporting = false;
          s_orthoDoneMsg.clear();
        }
        if (G.hStat)
          SetWindowTextW(G.hStat, L"\u5c31\u7eea");
      }
      if (s_panoJustDone) {
        s_panoJustDone = false;
      }
      Sleep(1); // 让出时间片给 Win32 消息泵，避免"未响应"
    } else {
      Sleep(50); // 最小化时减少 CPU 占用
    }
  }
  // 退出前清理：确保 viewer 真正停止
  FLOG("[MAIN] Exiting main loop after %d frames, s_userWantsQuit=%d\n",
       frameCount, (int)s_userWantsQuit);
  viewer->setDone(true);

  DeleteObject(hBrBg);
  DeleteObject(hBrTool);
  DeleteObject(hBrSide);
  CoUninitialize();
  FLOG("[MAIN] Program exiting normally\n");
  if (s_logFile)
    fclose(s_logFile);
  return 0;
}
