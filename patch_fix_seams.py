"""
Fix ortho seam lines and pano black areas:
1. Ortho: fix Y-copy logic — flip source rows to make buffer top-down,
   then write TIFF rows in order (top-down matches TIFF native order)
2. Pano: revert face size to viewport size (FBO not working without attach),
   keep 12K output via bilinear upscale; remove FBO code
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
# FIX 1: Ortho — fix pixel copy Y-order
# Revert dstOffY to simple s_subRow*vpH, but FLIP source rows
# so buffer is top-down. Then use a top-down writeTiff.
# ══════════════════════════════════════════════════════════════

old_copy = """  // ── 将 viewport 像素拷贝到输出分块缓冲区 ──
  int vpW = G.vpW, vpH = G.vpH;
  const unsigned char *src = s_orthoCapture->data();
  int dstOffX = s_subCol * vpW;
  // subRow=0 是最北的条带，在bottom-up缓冲区中应放在最高行
  int dstOffY = (s_subRows - 1 - s_subRow) * vpH;
  for (int row = 0; row < vpH; row++) {
    int dstY = dstOffY + row;
    if (dstY >= s_curOutTileH)
      break;
    int copyW = std::min(vpW, s_curOutTileW - dstOffX);
    if (copyW <= 0)
      break;
    memcpy(&s_tileBuf[((size_t)dstY * s_curOutTileW + dstOffX) * 3],
           &src[(size_t)row * vpW * 3], (size_t)copyW * 3);
  }"""

new_copy = """  // ── 将 viewport 像素拷贝到输出分块缓冲区（top-down 布局）──
  int vpW = G.vpW, vpH = G.vpH;
  const unsigned char *src = s_orthoCapture->data();
  int dstOffX = s_subCol * vpW;
  int dstOffY = s_subRow * vpH;
  for (int row = 0; row < vpH; row++) {
    int dstY = dstOffY + row;
    if (dstY >= s_curOutTileH)
      break;
    int srcRow = vpH - 1 - row; // 翻转：osg bottom-up → 缓冲区 top-down
    int copyW = std::min(vpW, s_curOutTileW - dstOffX);
    if (copyW <= 0)
      break;
    memcpy(&s_tileBuf[((size_t)dstY * s_curOutTileW + dstOffX) * 3],
           &src[(size_t)srcRow * vpW * 3], (size_t)copyW * 3);
  }"""

if old_copy in code:
    code = code.replace(old_copy, new_copy)
    changes += 1
    print("OK: fixed ortho pixel copy Y-order (top-down buffer)")
else:
    print("ERROR: ortho copy pattern not found")

# ══════════════════════════════════════════════════════════════
# FIX 2: Ortho — write tile buffer as top-down to TIFF
# The tile buffer is now top-down, writeTiff expects bottom-up.
# Replace the writeTiff call for tile buffer with direct top-down write.
# ══════════════════════════════════════════════════════════════

old_write = """  bool ok = writeTiff(tilePath, s_tileBuf.data(), s_curOutTileW, s_curOutTileH);"""

new_write = """  // tileBuf 是 top-down 布局，直接按序写入 TIFF（TIFF 原生也是 top-down）
  bool ok = false;
  {
    std::ofstream out(W(tilePath), std::ios::binary);
    if (out) {
      int w = s_curOutTileW, h = s_curOutTileH;
      auto w16 = [&](uint16_t v) { out.write((char *)&v, 2); };
      auto w32 = [&](uint32_t v) { out.write((char *)&v, 4); };
      uint32_t stripBytes = (uint32_t)w * h * 3;
      uint32_t bpsOff = 158, xresOff = bpsOff + 6;
      uint32_t yresOff = xresOff + 8, stripOff = yresOff + 8;
      w16(0x4949); w16(42); w32(8);
      w16(12);
      auto entry = [&](uint16_t tag, uint16_t type, uint32_t cnt, uint32_t val) {
        w16(tag); w16(type); w32(cnt); w32(val);
      };
      entry(256, 4, 1, (uint32_t)w);
      entry(257, 4, 1, (uint32_t)h);
      entry(258, 3, 3, bpsOff);
      entry(259, 3, 1, 1);
      entry(262, 3, 1, 2);
      entry(273, 4, 1, stripOff);
      entry(277, 3, 1, 3);
      entry(278, 4, 1, (uint32_t)h);
      entry(279, 4, 1, stripBytes);
      entry(282, 5, 1, xresOff);
      entry(283, 5, 1, yresOff);
      entry(296, 3, 1, 2);
      w32(0);
      w16(8); w16(8); w16(8);
      w32(72); w32(1); w32(72); w32(1);
      // top-down 写入（缓冲区已经是 top-down，直接按序写入）
      for (int row = 0; row < h; row++)
        out.write((const char *)(s_tileBuf.data() + (size_t)row * w * 3), w * 3);
      ok = out.good();
    }
  }"""

if old_write in code:
    code = code.replace(old_write, new_write)
    changes += 1
    print("OK: replaced writeTiff with top-down write for tile buffer")
else:
    print("ERROR: writeTiff call pattern not found")

# ══════════════════════════════════════════════════════════════
# FIX 3: Pano — revert face size to viewport, remove FBO
# ══════════════════════════════════════════════════════════════

# Revert face size
old_facesize = "s_panoFaceSize = PANO_FACE_SIZE;"
new_facesize = "s_panoFaceSize = std::min(G.vpW, G.vpH); // 使用窗口大小，避免超出 framebuffer"
if old_facesize in code:
    code = code.replace(old_facesize, new_facesize)
    changes += 1
    print("OK: reverted pano face size to viewport size")
else:
    print("WARN: pano face size pattern not found")

# Remove FBO enable
fbo_enable = """
  // 启用 FBO 离屏渲染（面尺寸可能超过窗口 framebuffer）
  G.viewer->getCamera()->setRenderTargetImplementation(
      osg::Camera::FRAME_BUFFER_OBJECT);
"""
if fbo_enable in code:
    code = code.replace(fbo_enable, "\n")
    changes += 1
    print("OK: removed FBO enable")
else:
    print("WARN: FBO enable pattern not found")

# Remove FBO disable
fbo_disable_old = """  // 恢复相机（关闭 FBO，回到窗口 framebuffer）
  G.viewer->getCamera()->setRenderTargetImplementation(
      osg::Camera::FRAME_BUFFER);
  G.viewer->getCamera()->setViewport(0, 0, G.vpW, G.vpH);"""

fbo_disable_new = """  // 恢复相机
  G.viewer->getCamera()->setViewport(0, 0, G.vpW, G.vpH);"""

if fbo_disable_old in code:
    code = code.replace(fbo_disable_old, fbo_disable_new)
    changes += 1
    print("OK: removed FBO disable")
else:
    print("WARN: FBO disable pattern not found")

print(f"\nTotal changes: {changes}")

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print(f"OK: wrote {SRC}")
