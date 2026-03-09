"""
Fix post-export crash: replace decomposed frame loop with normal frame() + UpdateCallback.

The decomposed frame calls (advance/event/update/render separately) corrupt
the viewer's internal state. Instead, use camera->setInitialDrawCallback()
which runs during the draw phase, and normal frame() calls.
"""

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

# Find the doOrthoExport function
START = "static void doOrthoExport() {"
idx = code.find(START)
if idx < 0:
    print("ERROR: doOrthoExport not found")
    exit(1)

# Find the closing brace
brace_count = 0
func_end = -1
for i in range(idx, len(code)):
    if code[i] == '{':
        brace_count += 1
    elif code[i] == '}':
        brace_count -= 1
        if brace_count == 0:
            func_end = i + 1
            break

# Also include the s_exporting line before the function
static_start = code.rfind("static bool s_exporting", 0, idx)
if static_start < 0:
    static_start = idx

print(f"Replacing doOrthoExport: chars {static_start} to {func_end}")

NEW_FUNC = r'''static bool s_exporting = false;

// 正射导出用的参数（主循环中使用）
static bool s_orthoActive = false;
static int  s_orthoFrame = 0;
static int  s_orthoTotal = 30;
static double s_oL, s_oR, s_oB, s_oT, s_near, s_far;
static osg::Vec3d s_eye, s_at, s_up;
static osg::Matrixd s_savedProj;
static std::string s_savePath;
static double s_worldL, s_worldT, s_rW, s_rH;

// 在主循环的 frame() 之前调用
static void orthoPreFrame() {
  if (!s_orthoActive) return;

  // 覆盖操纵器设置的矩阵为正射
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      s_oL, s_oR, s_oB, s_oT, s_near, s_far);
  G.viewer->getCamera()->setViewMatrixAsLookAt(s_eye, s_at, s_up);

  s_orthoFrame++;
  if (G.hStat && (s_orthoFrame % 5 == 0))
    SetWindowTextW(G.hStat,
        (L"\u6b63\u5728\u6e32\u67d3... " + std::to_wstring(s_orthoFrame) +
         L"/" + std::to_wstring(s_orthoTotal)).c_str());
}

// 在主循环的 frame() 之后调用
static void orthoPostFrame() {
  if (!s_orthoActive) return;
  if (s_orthoFrame < s_orthoTotal) return; // 继续渲染

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

  // 再覆盖一次矩阵并渲染最后一帧截图
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      s_oL, s_oR, s_oB, s_oT, s_near, s_far);
  G.viewer->getCamera()->setViewMatrixAsLookAt(s_eye, s_at, s_up);
  G.viewer->frame();

  G.viewer->getCamera()->setFinalDrawCallback(nullptr);

  // 恢复投影矩阵
  G.viewer->getCamera()->setProjectionMatrix(s_savedProj);
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

    if (G.hStat)
      SetWindowTextW(G.hStat, (L"\u6b63\u5c04\u56fe\u50cf\u5df2\u4fdd\u5b58: " +
                               W(s_savePath)).c_str());
    MessageBoxW(
        G.hMain,
        (L"\u6b63\u5c04\u56fe\u50cf\u5df2\u6210\u529f\u5bfc\u51fa\uff01\n\n"
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

static void doOrthoExport() {
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

  // 从当前透视相机计算可视范围
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
  s_orthoFrame = 0;

  // 保存投影矩阵
  s_savedProj = G.viewer->getCamera()->getProjectionMatrix();

  // 正射参数（眼空间坐标）
  double sceneH = bs.radius() * 4.0;
  double cz = center.z();
  s_oL = -halfW; s_oR = halfW;
  s_oB = -halfH; s_oT = halfH;
  s_near = 0.1;  s_far = sceneH * 2.0;
  s_eye = osg::Vec3d(center.x(), center.y(), cz + sceneH * 0.9);
  s_at  = osg::Vec3d(center.x(), center.y(), cz);
  s_up  = osg::Vec3d(0, 1, 0);
  s_savePath = savePath;

  // 世界坐标（用于 TFW）
  s_worldL = center.x() - halfW;
  s_worldT = center.y() + halfH;
  s_rW = halfW * 2.0;
  s_rH = halfH * 2.0;

  if (G.hStat)
    SetWindowTextW(G.hStat, L"\u6b63\u5728\u6e32\u67d3\u6b63\u5c04\u56fe\u50cf...");
  // 后续渲染由主循环中的 orthoPreFrame/orthoPostFrame 驱动
}
'''

code = code[:static_start] + NEW_FUNC + "\n" + code[func_end:]

# Now patch the main loop to call orthoPreFrame/orthoPostFrame
# Find the main loop section
main_loop_marker = "updateInfoHud();"
ml_idx = code.find(main_loop_marker)
if ml_idx < 0:
    print("ERROR: main loop marker not found")
    exit(1)

# Find the frame() call after updateInfoHud
frame_line_start = code.find("viewer->frame();", ml_idx)
if frame_line_start < 0:
    print("ERROR: frame() call not found in main loop")
    exit(1)

# Replace the two lines:
#       updateInfoHud();
#       viewer->frame();
# with:
#       updateInfoHud();
#       orthoPreFrame();
#       viewer->frame();
#       orthoPostFrame();

old_loop = "updateInfoHud();\n      viewer->frame();"
new_loop = "updateInfoHud();\n      orthoPreFrame();\n      viewer->frame();\n      orthoPostFrame();"

if old_loop in code:
    code = code.replace(old_loop, new_loop)
    print("OK: patched main loop with orthoPreFrame/orthoPostFrame")
else:
    print("ERROR: main loop pattern not found, trying alternate...")
    # Try with different indentation
    old_loop2 = "updateInfoHud();\n    viewer->frame();"
    new_loop2 = "updateInfoHud();\n    orthoPreFrame();\n    viewer->frame();\n    orthoPostFrame();"
    if old_loop2 in code:
        code = code.replace(old_loop2, new_loop2)
        print("OK: patched main loop (alternate indent)")
    else:
        print("FAILED: could not find main loop pattern")
        # Show context
        area = code[ml_idx:ml_idx+200]
        print(f"Context: {repr(area)}")

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("OK: complete rewrite - no more decomposed frame calls")
