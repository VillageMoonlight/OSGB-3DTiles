"""
Patch osgb_viewer.cpp: replace FBO ortho export with glReadPixels approach.
Handles both CRLF and LF line endings.
"""

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")

# Normalize to LF for matching, then restore original line ending
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

# --- Markers ---
START_MARKER = "s_exporting = true;\n"
END_MARKER = "// \u4e3b\u76f8\u673a\u77e9\u9635"  # 主相机矩阵

start_idx = code.find(START_MARKER)
end_idx = code.find(END_MARKER)

if start_idx < 0:
    print("ERROR: START_MARKER not found")
    exit(1)
if end_idx < 0:
    print("ERROR: END_MARKER not found")
    exit(1)

# Include the END_MARKER line
end_line_end = code.index("\n", end_idx) + 1

old = code[start_idx:end_line_end]
print(f"Found block: {len(old)} chars, from pos {start_idx} to {end_line_end}")

new_block = """s_exporting = true;
  if (G.hStat)
    SetWindowTextW(G.hStat,
                   L"\\u6b63\\u5728\\u6e32\\u67d3\\u6b63\\u5c04\\u56fe\\u50cf...");

  double sceneH = bs.radius() * 4.0;
  double cz = center.z();
  osg::Vec3d eye(center.x(), center.y(), cz + sceneH * 0.9);
  osg::Vec3d at(center.x(), center.y(), cz);
  osg::Vec3d up(0, 1, 0);

  // \u4f7f\u7528\u7a97\u53e3\u5206\u8fa8\u7387\uff08\u4e3b\u76f8\u673a\u76f4\u63a5\u622a\u56fe\uff0c\u6700\u53ef\u9760\uff09
  outW = G.vpW;
  outH = G.vpH;

  // \u2500\u2500 \u6e32\u67d3 30 \u5e27\u8ba9 PagedLOD \u5b8c\u5168\u52a0\u8f7d \u2500\u2500
  // \u5206\u89e3\u5e27: advance\u2192event\u2192update\u2192\u8986\u76d6\u77e9\u9635\u2192render
  const int N = 30;
  for (int i = 0; i < N; i++) {
    G.viewer->advance();
    G.viewer->eventTraversal();
    G.viewer->updateTraversal();
    // \u5728 update \u540e\u8986\u76d6\u64cd\u7eb5\u5668\u8bbe\u7f6e\u7684\u77e9\u9635\u4e3a\u6b63\u5c04
    G.viewer->getCamera()->setProjectionMatrixAsOrtho(
        oL, oR, oB, oT, cz - sceneH, cz + sceneH);
    G.viewer->getCamera()->setViewMatrixAsLookAt(eye, at, up);
    G.viewer->renderingTraversals();
    if (G.hStat && (i % 5 == 0))
      SetWindowTextW(G.hStat,
          (L"\\u6b63\\u5728\\u6e32\\u67d3... " + std::to_wstring(i + 1) +
           L"/" + std::to_wstring(N)).c_str());
  }

  // \u2500\u2500 \u6700\u540e\u4e00\u5e27\uff1a\u901a\u8fc7 FinalDrawCallback + glReadPixels \u622a\u53d6\u50cf\u7d20 \u2500\u2500
  osg::ref_ptr<osg::Image> img = new osg::Image();
  img->allocateImage(outW, outH, 1, GL_RGB, GL_UNSIGNED_BYTE);

  struct ReadPixelsCB : public osg::Camera::DrawCallback {
    osg::Image *dst; int w, h;
    ReadPixelsCB(osg::Image *d, int ww, int hh) : dst(d), w(ww), h(hh) {}
    void operator()(osg::RenderInfo &) const override {
      glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, dst->data());
    }
  };
  G.viewer->getCamera()->setFinalDrawCallback(
      new ReadPixelsCB(img.get(), outW, outH));

  // \u518d\u6e32\u67d3\u4e00\u5e27\uff0c\u7531\u56de\u8c03\u622a\u56fe
  G.viewer->advance();
  G.viewer->eventTraversal();
  G.viewer->updateTraversal();
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      oL, oR, oB, oT, cz - sceneH, cz + sceneH);
  G.viewer->getCamera()->setViewMatrixAsLookAt(eye, at, up);
  G.viewer->renderingTraversals();

  G.viewer->getCamera()->setFinalDrawCallback(nullptr);
  s_exporting = false;
  // \u4e3b\u76f8\u673a\u77e9\u9635\u5728\u4e0b\u4e00\u5e27\u7531\u64cd\u7eb5\u5668\u81ea\u52a8\u6062\u590d
"""

code = code[:start_idx] + new_block + code[end_line_end:]

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("OK: patched doOrthoExport - FBO removed, glReadPixels added")
