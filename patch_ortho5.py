"""
Add dual-mode ortho export:
  1. "正交截图" (ID_ORTHO_EXPORT=1401) - ortho projection from current viewpoint
  2. "正射DOM" (ID_ORTHO_DOM=1402) - true nadir ortho from straight above

Key changes:
  - Add ID_ORTHO_DOM=1402
  - Add DOM button + menu item
  - Add WM_COMMAND handler for DOM
  - Modify doOrthoExport to accept a 'nadir' parameter
  - Use scene UpdateCallback (runs AFTER manipulator) to override view matrix for DOM mode
  - Remove orthoPreFrame (not needed - UpdateCallback does the job for both modes)
"""

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

changes = 0

# 1. Add ID_ORTHO_DOM after ID_ORTHO_EXPORT
old = '#define ID_ORTHO_EXPORT 1401'
new = '#define ID_ORTHO_EXPORT 1401\n#define ID_ORTHO_DOM    1402'
if old in code:
    code = code.replace(old, new)
    changes += 1
    print("[1] Added ID_ORTHO_DOM")

# 2. Add DOM button to toolbar
old_btn = '{ID_ORTHO_EXPORT, L"\\U0001f4f7 \\u6b63\\u5c04", nullptr},'
new_btn = '{ID_ORTHO_EXPORT, L"\\U0001f4f7 \\u6b63\\u5c04", nullptr},\n      {ID_ORTHO_DOM, L"\\U0001f5fa DOM", nullptr},'
if old_btn in code:
    code = code.replace(old_btn, new_btn)
    changes += 1
    print("[2] Added DOM toolbar button")

# 3. Add DOM menu item
old_menu = 'AppendMenuW(mT, MF_STRING, ID_ORTHO_EXPORT,\n              L"\\u6b63\\u5c04\\u8f93\\u51fa(\\u0026O)...\\tCtrl+P");'
new_menu = ('AppendMenuW(mT, MF_STRING, ID_ORTHO_EXPORT,\n'
            '              L"\\u6b63\\u4ea4\\u622a\\u56fe(\\u0026O)...");\n'
            '  AppendMenuW(mT, MF_STRING, ID_ORTHO_DOM,\n'
            '              L"\\u6b63\\u5c04DOM(\\u0026D)...");')
if old_menu in code:
    code = code.replace(old_menu, new_menu)
    changes += 1
    print("[3] Added DOM menu item")

# 4. Add WM_COMMAND handler for DOM
old_cmd = '    case ID_ORTHO_EXPORT:\n      doOrthoExport();\n      break;'
new_cmd = ('    case ID_ORTHO_EXPORT:\n      doOrthoExport(false);\n      break;\n'
           '    case ID_ORTHO_DOM:\n      doOrthoExport(true);\n      break;')
if old_cmd in code:
    code = code.replace(old_cmd, new_cmd)
    changes += 1
    print("[4] Added WM_COMMAND handler for DOM")

# 5. Rewrite doOrthoExport and related functions
# Find and replace the entire export section

START = "static bool s_exporting = false;"
idx = code.find(START)
if idx < 0:
    print("ERROR: s_exporting not found")
    exit(1)

# Find the loadOsgb function which comes after doOrthoExport
load_marker = "static void loadOsgb"
load_idx = code.find(load_marker, idx)
if load_idx < 0:
    print("ERROR: loadOsgb not found")
    exit(1)

# Find the last newline before loadOsgb
cut_end = code.rfind("\n", 0, load_idx) + 1

print(f"[5] Replacing export section: chars {idx} to {cut_end}")

NEW_EXPORT = r'''static bool s_exporting = false;
static bool s_orthoActive = false;
static bool s_orthoNadir = false;  // true=DOM false=正交截图
static int  s_orthoFrame = 0;
static int  s_orthoTotal = 30;
static double s_oL, s_oR, s_oB, s_oT, s_near, s_far;
static osg::Vec3d s_eye, s_at, s_up;
static osg::Matrixd s_savedProj;
static std::string s_savePath;
static double s_worldL, s_worldT, s_rW, s_rH;

// 场景图 UpdateCallback —— 在操纵器 updateCamera 之后执行
// 用于 DOM 模式覆盖视图矩阵，和两种模式覆盖投影矩阵
struct OrthoOverrideCB : public osg::NodeCallback {
  void operator()(osg::Node *node, osg::NodeVisitor *nv) override {
    traverse(node, nv);
    if (!s_orthoActive) return;
    // 覆盖投影矩阵为正射（两种模式都需要）
    G.viewer->getCamera()->setProjectionMatrixAsOrtho(
        s_oL, s_oR, s_oB, s_oT, s_near, s_far);
    // DOM 模式：覆盖视图矩阵为正上方俯视
    // 正交截图模式：保留操纵器设置的视角（不覆盖）
    if (s_orthoNadir)
      G.viewer->getCamera()->setViewMatrixAsLookAt(s_eye, s_at, s_up);

    s_orthoFrame++;
    if (G.hStat && (s_orthoFrame % 5 == 0)) {
      auto label = s_orthoNadir ? L"\u6e32\u67d3DOM... " : L"\u6e32\u67d3\u6b63\u4ea4\u622a\u56fe... ";
      SetWindowTextW(G.hStat,
          (label + std::to_wstring(s_orthoFrame) +
           L"/" + std::to_wstring(s_orthoTotal)).c_str());
    }
  }
};
static osg::ref_ptr<OrthoOverrideCB> s_overrideCB;

// 在主循环 frame() 之后调用
static void orthoPostFrame() {
  if (!s_orthoActive) return;
  if (s_orthoFrame < s_orthoTotal) return;

  // 已渲染足够帧数 → 截图
  int outW = G.vpW, outH = G.vpH;
  osg::ref_ptr<osg::Image> img = new osg::Image();
  img->allocateImage(outW, outH, 1, GL_RGB, GL_UNSIGNED_BYTE);

  struct ReadCB : public osg::Camera::DrawCallback {
    osg::Image *dst; int w, h;
    ReadCB(osg::Image *d, int ww, int hh) : dst(d), w(ww), h(hh) {}
    void operator()(osg::RenderInfo &) const override {
      dst->readPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE);
    }
  };
  G.viewer->getCamera()->setFinalDrawCallback(new ReadCB(img.get(), outW, outH));

  // 再渲染一帧截图 (UpdateCallback 仍然会覆盖矩阵)
  G.viewer->frame();

  // 清理
  G.viewer->getCamera()->setFinalDrawCallback(nullptr);
  G.viewer->getCamera()->setProjectionMatrix(s_savedProj);
  if (s_overrideCB && G.sceneRoot)
    G.sceneRoot->removeUpdateCallback(s_overrideCB.get());
  s_orthoActive = false;
  s_exporting = false;

  // 写 TIFF
  bool ok = writeTiff(s_savePath, img->data(), outW, outH);
  if (ok) {
    auto stem = [](const std::string &p) {
      auto d = p.rfind('.');
      return d != std::string::npos ? p.substr(0, d) : p;
    };
    writeTfw(stem(s_savePath) + ".tfw", s_worldL, s_worldT,
             s_rW / outW, s_rH / outH);
    if (G.metaValid && !G.metaSrs.empty())
      writePrj(stem(s_savePath) + ".prj", G.metaSrs);

    auto modeW = s_orthoNadir ? L"\u6b63\u5c04DOM" : L"\u6b63\u4ea4\u622a\u56fe";
    if (G.hStat)
      SetWindowTextW(G.hStat, (std::wstring(modeW) + L"\u5df2\u4fdd\u5b58: " +
                               W(s_savePath)).c_str());
    MessageBoxW(
        G.hMain,
        (std::wstring(modeW) + L"\u5df2\u6210\u529f\u5bfc\u51fa\uff01\n\n"
         L"\u5206\u8fa8\u7387: " + std::to_wstring(outW) + L" \u00d7 " +
         std::to_wstring(outH) + L"\n" +
         L"\u6587\u4ef6: " + W(s_savePath)).c_str(),
        L"\u5bfc\u51fa\u5b8c\u6210", MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(G.hMain, L"\u5199\u5165 TIFF \u6587\u4ef6\u5931\u8d25\uff01",
                L"\u9519\u8bef", MB_OK | MB_ICONERROR);
  }
  if (G.hStat) SetWindowTextW(G.hStat, L"\u5c31\u7eea");
}

static void doOrthoExport(bool nadir) {
  if (s_exporting) return;
  if (!G.viewer || !G.sceneRoot) {
    MessageBoxW(G.hMain, L"\u8bf7\u5148\u6253\u5f00 OSGB \u6570\u636e\uff01",
                L"\u63d0\u793a", MB_OK | MB_ICONWARNING);
    return;
  }
  osg::BoundingSphere bs = G.sceneRoot->getBound();
  if (!bs.valid()) {
    MessageBoxW(G.hMain, L"\u573a\u666f\u65e0\u6709\u6548\u5305\u56f4\u76d2\uff01",
                L"\u9519\u8bef", MB_OK | MB_ICONERROR);
    return;
  }

  double fovy, aspect, zNear, zFar;
  G.viewer->getCamera()->getProjectionMatrixAsPerspective(fovy, aspect, zNear, zFar);
  osg::Vec3d center = G.manip->getCenter();
  double dist = G.manip->getDistance();
  double halfH = dist * tan(osg::DegreesToRadians(fovy * 0.5));
  double halfW = halfH * aspect;

  std::string savePath = pickSaveTif();
  if (savePath.empty()) return;

  s_exporting = true;
  s_orthoActive = true;
  s_orthoNadir = nadir;
  s_orthoFrame = 0;

  s_savedProj = G.viewer->getCamera()->getProjectionMatrix();

  double sceneH = bs.radius() * 4.0;
  double cz = center.z();
  s_oL = -halfW; s_oR = halfW;
  s_oB = -halfH; s_oT = halfH;
  s_near = 0.1;  s_far = sceneH * 2.0;
  s_eye = osg::Vec3d(center.x(), center.y(), cz + sceneH * 0.9);
  s_at  = osg::Vec3d(center.x(), center.y(), cz);
  s_up  = osg::Vec3d(0, 1, 0);
  s_savePath = savePath;

  s_worldL = center.x() - halfW;
  s_worldT = center.y() + halfH;
  s_rW = halfW * 2.0;
  s_rH = halfH * 2.0;

  // 安装 UpdateCallback（在操纵器更新相机之后覆盖矩阵）
  if (!s_overrideCB) s_overrideCB = new OrthoOverrideCB();
  G.sceneRoot->addUpdateCallback(s_overrideCB.get());

  auto label = nadir ? L"\u6b63\u5728\u6e32\u67d3\u6b63\u5c04DOM..." : L"\u6b63\u5728\u6e32\u67d3\u6b63\u4ea4\u622a\u56fe...";
  if (G.hStat) SetWindowTextW(G.hStat, label);
  // 后续渲染由主循环 frame() + orthoPostFrame() 驱动
}

'''

code = code[:idx] + NEW_EXPORT + code[cut_end:]

# 6. Remove the now-unused orthoPreFrame from main loop
old_loop = "orthoPreFrame();\n      viewer->frame();\n      orthoPostFrame();"
new_loop = "viewer->frame();\n      orthoPostFrame();"
if old_loop in code:
    code = code.replace(old_loop, new_loop)
    changes += 1
    print("[6] Removed orthoPreFrame from main loop")

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print(f"\nDone! Applied {changes + 2} changes:")
print("  - ID_ORTHO_DOM=1402")
print("  - Toolbar button: 🗺 DOM")
print("  - Menu: 工具 → 正交截图(&O) + 正射DOM(&D)")
print("  - WM_COMMAND: doOrthoExport(false) / doOrthoExport(true)")
print("  - OrthoOverrideCB: scene update callback overrides matrices AFTER manipulator")
print("  - Removed orthoPreFrame (UpdateCallback replaces it)")
