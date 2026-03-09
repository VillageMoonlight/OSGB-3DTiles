"""
Fix two bugs:
1. Ortho: projection bounds are global but eye is per-tile → use centered projection
2. Pano: face 3072x3072 exceeds window framebuffer → use FBO rendering
"""

SRC = r"src\osgb_viewer.cpp"
with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

changes = 0

# ══════════════════════════════════════════════════════════════
# FIX 1: Ortho projection — use centered bounds instead of global
# ══════════════════════════════════════════════════════════════
# The eye is at tileCX = sceneCenter + (s_oL+s_oR)/2
# So ortho bounds must be eye-relative (centered), not global

old_ortho_proj = """  // 设置当前子渲染的正射投影
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(s_oL, s_oR, s_oB, s_oT,
                                                    s_near, s_far);

  // 设置 ViewMatrix —— 始终正北朝上
  double tileCX = s_sceneCenter.x() + (s_oL + s_oR) * 0.5;
  double tileCY = s_sceneCenter.y() + (s_oB + s_oT) * 0.5;"""

new_ortho_proj = """  // 设置当前子渲染的正射投影（eye-relative centered bounds）
  double halfProjW = G.vpW * s_gsdWorld * 0.5;
  double halfProjH = G.vpH * s_gsdWorld * 0.5;
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(-halfProjW, halfProjW,
                                                    -halfProjH, halfProjH,
                                                    s_near, s_far);

  // 设置 ViewMatrix —— 始终正北朝上
  double tileCX = s_sceneCenter.x() + (s_oL + s_oR) * 0.5;
  double tileCY = s_sceneCenter.y() + (s_oB + s_oT) * 0.5;"""

if old_ortho_proj in code:
    code = code.replace(old_ortho_proj, new_ortho_proj)
    changes += 1
    print("OK: fixed ortho projection to centered bounds")
else:
    print("ERROR: ortho projection pattern not found")

# ══════════════════════════════════════════════════════════════
# FIX 2: Ortho tileBuf Y-order — subRow=0 is northernmost, should
#         go to HIGH rows in bottom-up buffer
# ══════════════════════════════════════════════════════════════

old_copy = """  int dstOffX = s_subCol * vpW;
  int dstOffY = s_subRow * vpH;"""

new_copy = """  int dstOffX = s_subCol * vpW;
  // subRow=0 是最北的条带，在bottom-up缓冲区中应放在最高行
  int dstOffY = (s_subRows - 1 - s_subRow) * vpH;"""

if old_copy in code:
    code = code.replace(old_copy, new_copy)
    changes += 1
    print("OK: fixed ortho tileBuf Y-ordering")
else:
    print("WARN: ortho copy pattern not found")

# ══════════════════════════════════════════════════════════════
# FIX 3: Pano — enable FBO rendering for large face sizes
# ══════════════════════════════════════════════════════════════

# In doPanoExport, after removing manipulator, add FBO setup
old_manip_remove = """  // 移除操纵器，直接控制相机
  G.viewer->setCameraManipulator(nullptr, false);"""

new_manip_remove = """  // 移除操纵器，直接控制相机
  G.viewer->setCameraManipulator(nullptr, false);

  // 启用 FBO 离屏渲染（面尺寸可能超过窗口 framebuffer）
  G.viewer->getCamera()->setRenderTargetImplementation(
      osg::Camera::FRAME_BUFFER_OBJECT);"""

if old_manip_remove in code:
    code = code.replace(old_manip_remove, new_manip_remove)
    changes += 1
    print("OK: added FBO enable in doPanoExport")
else:
    # Try alternate pattern
    alt = "G.viewer->setCameraManipulator(nullptr, false);"
    idx = code.find(alt)
    # Find the one in doPanoExport context
    pano_idx = code.find("static void doPanoExport()")
    if pano_idx > 0:
        manip_idx = code.find(alt, pano_idx)
        if manip_idx > 0:
            end_line = code.index("\n", manip_idx) + 1
            fbo_line = "\n  // 启用 FBO 离屏渲染（面尺寸可能超过窗口 framebuffer）\n  G.viewer->getCamera()->setRenderTargetImplementation(\n      osg::Camera::FRAME_BUFFER_OBJECT);\n"
            code = code[:end_line] + fbo_line + code[end_line:]
            changes += 1
            print("OK: added FBO enable (alternate)")
        else:
            print("ERROR: setCameraManipulator not found in doPanoExport")
    else:
        print("ERROR: doPanoExport not found")

# ══════════════════════════════════════════════════════════════
# FIX 4: Pano — restore from FBO after rendering
# ══════════════════════════════════════════════════════════════

# In panoPostFrame, before restoring viewport, disable FBO
old_restore = """  // 恢复相机
  G.viewer->getCamera()->setViewport(0, 0, G.vpW, G.vpH);
  G.viewer->getCamera()->setProjectionMatrix(s_panoProjSaved);"""

new_restore = """  // 恢复相机（关闭 FBO，回到窗口 framebuffer）
  G.viewer->getCamera()->setRenderTargetImplementation(
      osg::Camera::FRAME_BUFFER);
  G.viewer->getCamera()->setViewport(0, 0, G.vpW, G.vpH);
  G.viewer->getCamera()->setProjectionMatrix(s_panoProjSaved);"""

if old_restore in code:
    code = code.replace(old_restore, new_restore)
    changes += 1
    print("OK: added FBO disable in panoPostFrame")
else:
    print("ERROR: pano restore pattern not found")

print(f"\nTotal changes: {changes}")

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print(f"OK: wrote {SRC}")
