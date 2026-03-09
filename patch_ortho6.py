"""
Fix DOM nadir view: move matrix override into main loop (between update and render).
The UpdateCallback approach didn't reliably override after the manipulator.

Strategy:
  - In the main loop, when s_orthoActive, use decomposed frame (advance/event/update/render)
    and override matrices BETWEEN update and render.
  - When NOT exporting, use normal frame().
  - Remove the OrthoOverrideCB entirely (not needed).
"""

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

# 1. Replace the main loop rendering section
old_loop = """    if (!IsIconic(G.hMain)) {
      updateInfoHud();
      viewer->frame();
      orthoPostFrame();
    }"""

new_loop = """    if (!IsIconic(G.hMain)) {
      updateInfoHud();
      if (s_orthoActive) {
        // 分解帧：在 update 后、render 前覆盖矩阵
        viewer->advance();
        viewer->eventTraversal();
        viewer->updateTraversal();
        // 操纵器已在 update 中设置了透视矩阵
        // 此处覆盖投影矩阵为正射（两种模式都需要）
        viewer->getCamera()->setProjectionMatrixAsOrtho(
            s_oL, s_oR, s_oB, s_oT, s_near, s_far);
        // DOM 模式：也覆盖视图矩阵为正上方俯视
        if (s_orthoNadir)
          viewer->getCamera()->setViewMatrixAsLookAt(s_eye, s_at, s_up);
        viewer->renderingTraversals();
        s_orthoFrame++;
        if (G.hStat && (s_orthoFrame % 5 == 0)) {
          auto lb = s_orthoNadir ? L"\\u6e32\\u67d3DOM... "
                                 : L"\\u6e32\\u67d3\\u6b63\\u4ea4... ";
          SetWindowTextW(G.hStat,
              (lb + std::to_wstring(s_orthoFrame) +
               L"/" + std::to_wstring(s_orthoTotal)).c_str());
        }
      } else {
        viewer->frame();
      }
      orthoPostFrame();
    }"""

if old_loop in code:
    code = code.replace(old_loop, new_loop)
    print("[1] Patched main loop with decomposed frame for ortho")
else:
    print("ERROR: main loop pattern not found")
    # Debug
    idx = code.find("orthoPostFrame")
    if idx >= 0:
        print(f"Found at {idx}: {repr(code[idx-200:idx+100])}")
    exit(1)

# 2. Remove the OrthoOverrideCB struct and s_overrideCB (no longer needed)
# Find and remove from "// 场景图 UpdateCallback" to just before "// 在主循环 frame()"
cb_start = code.find("// \u573a\u666f\u56fe UpdateCallback")
cb_end = code.find("// \u5728\u4e3b\u5faa\u73af frame()")
if cb_start >= 0 and cb_end >= 0:
    code = code[:cb_start] + code[cb_end:]
    print("[2] Removed OrthoOverrideCB")
else:
    print("[2] WARN: Could not find OrthoOverrideCB to remove")

# 3. Remove the frame counter from OrthoOverrideCB (now in main loop)
#    But we also need to remove the UpdateCallback setup from doOrthoExport
old_setup = """  // 安装 UpdateCallback（在操纵器更新相机之后覆盖矩阵）
  if (!s_overrideCB) s_overrideCB = new OrthoOverrideCB();
  G.sceneRoot->addUpdateCallback(s_overrideCB.get());
"""
if old_setup in code:
    code = code.replace(old_setup, "")
    print("[3] Removed UpdateCallback setup from doOrthoExport")

# 4. Remove the UpdateCallback removal from orthoPostFrame
old_remove = """  if (s_overrideCB && G.sceneRoot)
    G.sceneRoot->removeUpdateCallback(s_overrideCB.get());
"""
if old_remove in code:
    code = code.replace(old_remove, "")
    print("[4] Removed UpdateCallback removal from orthoPostFrame")

# 5. Also remove the s_overrideCB static variable
old_var = "static osg::ref_ptr<OrthoOverrideCB> s_overrideCB;\n"
if old_var in code:
    code = code.replace(old_var, "")
    print("[5] Removed s_overrideCB variable")

# 6. In orthoPostFrame, the capture frame also needs the matrix override
# Find the "再渲染一帧截图" section and add matrix override before frame()
old_capture = """  // 再渲染一帧截图 (UpdateCallback 仍然会覆盖矩阵)
  G.viewer->frame();"""

new_capture = """  // 再渲染一帧截图（手动覆盖矩阵）
  G.viewer->advance();
  G.viewer->eventTraversal();
  G.viewer->updateTraversal();
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      s_oL, s_oR, s_oB, s_oT, s_near, s_far);
  if (s_orthoNadir)
    G.viewer->getCamera()->setViewMatrixAsLookAt(s_eye, s_at, s_up);
  G.viewer->renderingTraversals();"""

if old_capture in code:
    code = code.replace(old_capture, new_capture)
    print("[6] Fixed capture frame with matrix override")

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("\nDone! Key change: matrix override now happens BETWEEN updateTraversal")
print("and renderingTraversals, guaranteeing it runs AFTER the manipulator.")
