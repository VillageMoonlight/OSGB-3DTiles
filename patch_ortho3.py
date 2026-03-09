"""
Comprehensive fix for doOrthoExport:
1. Fix ortho projection bounds: use EYE-SPACE coordinates, not world coordinates
2. Fix near/far clip planes: use proper distances
3. Save/restore projection matrix (prevent black screen after export)
4. Keep world coordinates for TFW file generation
"""

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

# Find the doOrthoExport function body
START = "static void doOrthoExport() {"
idx = code.find(START)
if idx < 0:
    print("ERROR: doOrthoExport not found")
    exit(1)

# Find the closing brace by counting braces
brace_count = 0
func_start = idx + len(START)
func_end = -1
for i in range(idx, len(code)):
    if code[i] == '{':
        brace_count += 1
    elif code[i] == '}':
        brace_count -= 1
        if brace_count == 0:
            func_end = i + 1
            break

if func_end < 0:
    print("ERROR: could not find end of doOrthoExport")
    exit(1)

# Find where the static bool line before the function starts
static_line_start = code.rfind("static bool s_exporting", 0, idx)
if static_line_start < 0:
    static_line_start = idx

print(f"Replacing doOrthoExport: chars {static_line_start} to {func_end}")

NEW_FUNC = r'''static bool s_exporting = false;
static void doOrthoExport() {
  if (s_exporting) return;
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

  // 从当前透视相机计算可视范围（世界坐标，用于 TFW）
  double fovy, aspect, zNear, zFar;
  G.viewer->getCamera()->getProjectionMatrixAsPerspective(fovy, aspect, zNear,
                                                          zFar);
  osg::Vec3d center = G.manip->getCenter();
  double dist = G.manip->getDistance();
  double halfH = dist * tan(osg::DegreesToRadians(fovy * 0.5));
  double halfW = halfH * aspect;

  // 世界坐标范围（用于 TFW 地理配准）
  double worldL = center.x() - halfW, worldR = center.x() + halfW;
  double worldB = center.y() - halfH, worldT = center.y() + halfH;
  double rW = worldR - worldL, rH = worldT - worldB;

  std::string savePath = pickSaveTif();
  if (savePath.empty()) return;

  s_exporting = true;
  if (G.hStat)
    SetWindowTextW(G.hStat,
                   L"\u6b63\u5728\u6e32\u67d3\u6b63\u5c04\u56fe\u50cf...");

  // ── 保存主相机投影矩阵（导出后恢复，防止黑屏）──
  osg::Matrixd savedProj = G.viewer->getCamera()->getProjectionMatrix();

  // 正射相机参数（眼空间坐标！）
  double sceneH = bs.radius() * 4.0;
  double cz = center.z();
  osg::Vec3d eye(center.x(), center.y(), cz + sceneH * 0.9);
  osg::Vec3d at(center.x(), center.y(), cz);
  osg::Vec3d up(0, 1, 0);

  // ── 渲染 30 帧让 PagedLOD 完全加载 ──
  const int N = 30;
  for (int i = 0; i < N; i++) {
    G.viewer->advance();
    G.viewer->eventTraversal();
    G.viewer->updateTraversal();
    // 覆盖操纵器的透视矩阵 → 正射投影（眼空间坐标）
    G.viewer->getCamera()->setProjectionMatrixAsOrtho(
        -halfW, halfW, -halfH, halfH, 0.1, sceneH * 2.0);
    G.viewer->getCamera()->setViewMatrixAsLookAt(eye, at, up);
    G.viewer->renderingTraversals();
    if (G.hStat && (i % 5 == 0))
      SetWindowTextW(G.hStat,
          (L"\u6b63\u5728\u6e32\u67d3... " + std::to_wstring(i + 1) +
           L"/" + std::to_wstring(N)).c_str());
  }

  // ── 最后一帧：截取像素 ──
  int outW = G.vpW, outH = G.vpH;
  osg::ref_ptr<osg::Image> img = new osg::Image();
  img->allocateImage(outW, outH, 1, GL_RGB, GL_UNSIGNED_BYTE);

  struct ReadPixelsCB : public osg::Camera::DrawCallback {
    osg::Image *dst; int w, h;
    ReadPixelsCB(osg::Image *d, int ww, int hh) : dst(d), w(ww), h(hh) {}
    void operator()(osg::RenderInfo &) const override {
      dst->readPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE);
    }
  };
  G.viewer->getCamera()->setFinalDrawCallback(
      new ReadPixelsCB(img.get(), outW, outH));

  G.viewer->advance();
  G.viewer->eventTraversal();
  G.viewer->updateTraversal();
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      -halfW, halfW, -halfH, halfH, 0.1, sceneH * 2.0);
  G.viewer->getCamera()->setViewMatrixAsLookAt(eye, at, up);
  G.viewer->renderingTraversals();

  G.viewer->getCamera()->setFinalDrawCallback(nullptr);

  // ── 恢复投影矩阵（防止黑屏）──
  G.viewer->getCamera()->setProjectionMatrix(savedProj);
  s_exporting = false;

  // 写 TIFF
  bool ok = writeTiff(savePath, img->data(), outW, outH);
  if (ok) {
    auto stem = [](const std::string &p) {
      auto d = p.rfind('.');
      return d != std::string::npos ? p.substr(0, d) : p;
    };
    writeTfw(stem(savePath) + ".tfw", worldL, worldT, rW / outW, rH / outH);
    if (G.metaValid && !G.metaSrs.empty())
      writePrj(stem(savePath) + ".prj", G.metaSrs);

    if (G.hStat)
      SetWindowTextW(G.hStat, (L"\u6b63\u5c04\u56fe\u50cf\u5df2\u4fdd\u5b58: " +
                               W(savePath))
                                  .c_str());
    MessageBoxW(
        G.hMain,
        (L"\u6b63\u5c04\u56fe\u50cf\u5df2\u6210\u529f\u5bfc\u51fa\uff01\n\n"
         L"\u5206\u8fa8\u7387: " +
         std::to_wstring(outW) + L" \u00d7 " + std::to_wstring(outH) + L"\n" +
         L"\u6587\u4ef6: " + W(savePath))
            .c_str(),
        L"\u5bfc\u51fa\u5b8c\u6210", MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(G.hMain,
                L"\u5199\u5165 TIFF \u6587\u4ef6\u5931\u8d25\uff01",
                L"\u9519\u8bef", MB_OK | MB_ICONERROR);
  }
  if (G.hStat) SetWindowTextW(G.hStat, L"\u5c31\u7eea");
}
'''

code = code[:static_line_start] + NEW_FUNC + "\n" + code[func_end:]

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("OK: doOrthoExport completely rewritten")
print("  - Ortho bounds now in EYE SPACE (-halfW..halfW, -halfH..halfH)")
print("  - Near/far: 0.1 .. sceneH*2.0")
print("  - Projection matrix saved/restored (no more black screen)")
print("  - Removed resolution dialog (uses window resolution)")
print("  - World coordinates preserved for TFW")
