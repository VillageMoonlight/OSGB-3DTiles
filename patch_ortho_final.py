"""
Final fix: Line-based approach to avoid Unicode matching issues.
Completely remove decomposed frame from main loop.
"""

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

lines = code.split("\n")

# 1. Find main loop: look for "s_orthoActive" in context of the main loop
main_loop_start = -1
main_loop_end = -1
for i, line in enumerate(lines):
    if "if (s_orthoActive)" in line and i > 1000:  # main loop is near end of file
        # Go back to find "if (!IsIconic"
        for j in range(i, i-10, -1):
            if "!IsIconic" in lines[j]:
                main_loop_start = j
                break
        # Go forward to find the closing brace
        brace_depth = 0
        for j in range(main_loop_start, len(lines)):
            brace_depth += lines[j].count("{") - lines[j].count("}")
            if brace_depth == 0 and j > main_loop_start:
                main_loop_end = j
                break
        break

if main_loop_start < 0:
    print("ERROR: Could not find main loop s_orthoActive block")
    exit(1)

print(f"Main loop block: lines {main_loop_start+1} to {main_loop_end+1}")
print(f"  Old content ({main_loop_end - main_loop_start + 1} lines):")
for k in range(main_loop_start, min(main_loop_end+1, main_loop_start+5)):
    print(f"    {k+1}: {lines[k][:60]}")

# Replace the entire block with simple version
new_block = [
    "    if (!IsIconic(G.hMain)) {",
    "      updateInfoHud();",
    "      orthoPreFrame();",
    "      viewer->frame();",
    "      orthoPostFrame();",
    "    }",
]

lines[main_loop_start:main_loop_end+1] = new_block
print(f"[1] Replaced main loop with simple version ({len(new_block)} lines)")

# 2. Find and replace orthoPostFrame and add orthoPreFrame
# Find "// \u5728\u4e3b\u5faa\u73af frame()" marker (在主循环 frame())
postframe_line = -1
for i, line in enumerate(lines):
    if "orthoPostFrame()" in line and "static void" in line:
        postframe_line = i
        break

if postframe_line < 0:
    # Try another approach: find the function by looking for its declaration
    for i, line in enumerate(lines):
        if "void orthoPostFrame" in line:
            postframe_line = i
            break

# Find the comment before orthoPostFrame
comment_line = postframe_line
for j in range(postframe_line, max(0, postframe_line-5), -1):
    if "//" in lines[j]:
        comment_line = j
        break

# Find doOrthoExport function
do_export_line = -1
for i, line in enumerate(lines):
    if "void doOrthoExport(bool nadir)" in line:
        do_export_line = i
        break

if postframe_line < 0 or do_export_line < 0:
    print(f"ERROR: postframe={postframe_line}, doExport={do_export_line}")
    exit(1)

print(f"orthoPostFrame at line {postframe_line+1}, doOrthoExport at line {do_export_line+1}")

# Generate the new pre/post frame functions
new_funcs = r"""// 保存操纵器状态用于恢复
static osg::Vec3d s_savedManipCenter;
static double     s_savedManipDist;
static osg::Quat  s_savedManipRot;

// 在主循环 frame() 之前调用
static void orthoPreFrame() {
  if (!s_orthoActive) return;

  // 设置正射投影（操纵器不会修改投影矩阵，所以这会保持到渲染）
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      s_oL, s_oR, s_oB, s_oT, s_near, s_far);

  // DOM 模式：通过操纵器设置俯视角（操纵器会在 frame 中正常应用）
  if (s_orthoNadir && G.manip) {
    G.manip->setCenter(s_at);
    G.manip->setDistance(s_far * 0.45);
    G.manip->setRotation(osg::Quat(osg::DegreesToRadians(-90.0), osg::Vec3d(1,0,0)));
  }

  s_orthoFrame++;
  if (G.hStat && (s_orthoFrame % 5 == 0)) {
    std::wstring lb = s_orthoNadir ? L"\u6e32\u67d3DOM... "
                                   : L"\u6e32\u67d3\u6b63\u4ea4... ";
    SetWindowTextW(G.hStat, (lb + std::to_wstring(s_orthoFrame) + L"/" +
                             std::to_wstring(s_orthoTotal)).c_str());
  }
}

// 在主循环 frame() 之后调用
static void orthoPostFrame() {
  if (!s_orthoActive) return;
  if (s_orthoFrame < s_orthoTotal) return;

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

  // 再渲染一帧截图
  orthoPreFrame();
  G.viewer->frame();

  G.viewer->getCamera()->setFinalDrawCallback(nullptr);
  G.viewer->getCamera()->setProjectionMatrix(s_savedProj);
  if (s_orthoNadir && G.manip) {
    G.manip->setCenter(s_savedManipCenter);
    G.manip->setDistance(s_savedManipDist);
    G.manip->setRotation(s_savedManipRot);
  }
  s_orthoActive = false;
  s_exporting = false;

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

""".split("\n")

lines[comment_line:do_export_line] = new_funcs
print(f"[2] Replaced orthoPostFrame section (lines {comment_line+1} to {do_export_line+1})")

# 3. In doOrthoExport, find "s_savedProj = " and add manipulator save after it
for i, line in enumerate(lines):
    if "s_savedProj = G.viewer->getCamera()->getProjectionMatrix();" in line:
        # Check if manipulator save already exists after this line
        if i+1 < len(lines) and "savedManipCenter" in lines[i+1]:
            print("[3] Manipulator save already exists, skipping")
        else:
            extra = [
                "",
                "  // \u4fdd\u5b58\u64cd\u7eb5\u5668\u72b6\u6001\uff08DOM \u6a21\u5f0f\u6062\u590d\u7528\uff09",
                "  if (G.manip) {",
                "    s_savedManipCenter = G.manip->getCenter();",
                "    s_savedManipDist   = G.manip->getDistance();",
                "    s_savedManipRot    = G.manip->getRotation();",
                "  }",
            ]
            lines[i+1:i+1] = extra
            print(f"[3] Added manipulator state save after line {i+1}")
        break

code = "\n".join(lines)
if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("\nDone! Main loop is now: orthoPreFrame → frame → orthoPostFrame")
