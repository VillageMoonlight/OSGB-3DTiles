"""
Patch panorama export: 12K output, 92° FOV overlap, bilinear interpolation,
fix nested frame() anti-pattern.
"""
import re

SRC = r"src\osgb_viewer.cpp"
with open(SRC, "rb") as f:
    raw = f.read()

code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

changes = 0

# ══════════════════════════════════════════════════════════════
# 1. Replace panoPreFrame — use PANO_FOV instead of 90.0
# ══════════════════════════════════════════════════════════════

old_pre = """  // 设置方形视口，确保 1:1 CubeMap 渲染无畸变
  int fs = s_panoFaceSize;
  G.viewer->getCamera()->setViewport(0, 0, fs, fs);

  // 设置 90° FOV 1:1 透视投影
  G.viewer->getCamera()->setProjectionMatrixAsPerspective(90.0, 1.0, 0.1,
                                                          s_panoFar);"""

new_pre = """  // 设置方形视口，确保 1:1 CubeMap 渲染无畸变
  int fs = s_panoFaceSize;
  G.viewer->getCamera()->setViewport(0, 0, fs, fs);

  // 设置 92° FOV 透视投影（>90°产生重叠区域，消除拼接缝）
  G.viewer->getCamera()->setProjectionMatrixAsPerspective(PANO_FOV, 1.0, 0.1,
                                                          s_panoFar);"""

if old_pre in code:
    code = code.replace(old_pre, new_pre)
    changes += 1
    print("OK: patched panoPreFrame FOV 90->PANO_FOV")
else:
    print("WARN: panoPreFrame FOV pattern not found")

# ══════════════════════════════════════════════════════════════
# 2. Add 12K constants and state variables after pano section header
# ══════════════════════════════════════════════════════════════

# Find "CubeMap 六面方向定义" and insert constants before it
cube_marker = "// CubeMap"
cube_idx = code.find(cube_marker)
if cube_idx < 0:
    print("ERROR: CubeMap marker not found")
    exit(1)

# Find the section header line before it (the "全景输出" line)
section_start = code.rfind("//", 0, cube_idx - 5)
# Go back to find the first // of the section
lines_before = code[:cube_idx].split("\n")
# Find where the section comment block starts
for i in range(len(lines_before)-1, -1, -1):
    stripped = lines_before[i].strip()
    if stripped and not stripped.startswith("//") and stripped != "":
        section_start_line = i + 1
        break

# Insert 12K constants after section header, before CubeMap
constants_block = """
// ── 12K 全景常量 ──
static const int PANO_OUT_W = 12288;    // 等距矩形输出宽度
static const int PANO_OUT_H = 6144;     // 等距矩形输出高度
static const int PANO_FACE_SIZE = 3072; // 每面渲染尺寸
static const double PANO_FOV = 92.0;    // 立方体面FOV（>90°产生重叠）
// FOV缩放因子：92°时投影范围为 [-tan(46°), tan(46°)]
static const double PANO_FOV_SCALE = 1.03553; // tan(46°)

"""

if "PANO_OUT_W" not in code:
    code = code[:cube_idx] + constants_block + code[cube_idx:]
    changes += 1
    print("OK: inserted 12K constants")
else:
    print("SKIP: 12K constants already present")

# ══════════════════════════════════════════════════════════════
# 3. Add pano capture state machine variables after s_panoJustDone
# ══════════════════════════════════════════════════════════════

capture_vars = """
// 回读状态机（与正射导出相同模式）
static osg::ref_ptr<osg::Image> s_panoCapture;
static bool s_panoCaptureReady = false;
"""

just_done_marker = 'static bool s_panoJustDone = false;'
if just_done_marker in code and "s_panoCaptureReady" not in code:
    idx = code.find(just_done_marker)
    end_of_line = code.index("\n", idx) + 1
    code = code[:end_of_line] + capture_vars + code[end_of_line:]
    changes += 1
    print("OK: inserted pano capture state variables")
else:
    print("SKIP: pano capture vars already present or marker not found")

# ══════════════════════════════════════════════════════════════
# 4. Replace cubemapToEquirect with bilinear interpolation + FOV scale
# ══════════════════════════════════════════════════════════════

# Find the entire cubemapToEquirect function
func_start_marker = "static osg::ref_ptr<osg::Image> cubemapToEquirect("
func_idx = code.find(func_start_marker)
if func_idx < 0:
    print("ERROR: cubemapToEquirect not found")
    exit(1)

# Find closing brace
brace = 0
func_end = -1
for i in range(func_idx, len(code)):
    if code[i] == '{':
        brace += 1
    elif code[i] == '}':
        brace -= 1
        if brace == 0:
            func_end = i + 1
            break

# Also include the comment line before it
comment_start = code.rfind("//", 0, func_idx)
comment_line_start = code.rfind("\n", 0, comment_start) + 1

new_cubemap = """// CubeMap → 等距矩形投影（双线性插值 + FOV 重叠缩放）
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
}"""

code = code[:comment_line_start] + new_cubemap + "\n" + code[func_end:]
changes += 1
print(f"OK: replaced cubemapToEquirect with bilinear+FOV version")

# ══════════════════════════════════════════════════════════════
# 5. Replace panoPostFrame — remove nested frame(), use DrawCallback state machine
# ══════════════════════════════════════════════════════════════

# Find panoPostFrame
pf_marker = "static void panoPostFrame() {"
pf_idx = code.find(pf_marker)
if pf_idx < 0:
    print("ERROR: panoPostFrame not found")
    exit(1)

brace = 0
pf_end = -1
for i in range(pf_idx, len(code)):
    if code[i] == '{':
        brace += 1
    elif code[i] == '}':
        brace -= 1
        if brace == 0:
            pf_end = i + 1
            break

new_panopost = """static void panoPostFrame() {
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
        PanoReadCB(osg::Image *d, int s, bool *flag) : dst(d), sz(s), readyFlag(flag) {}
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
  FLOG("[PANO] face %d captured (%dx%d)\\n", s_panoFace, fs, fs);

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

  // 恢复相机
  G.viewer->getCamera()->setViewport(0, 0, G.vpW, G.vpH);
  G.viewer->getCamera()->setProjectionMatrix(s_panoProjSaved);
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
    std::wstring msg =
        L"全景图已成功导出！\\n\\n";
    msg += L"分辨率: " + std::to_wstring(outW) + L" × " +
           std::to_wstring(outH) + L"\\n";
    msg += L"视点高度: " + std::to_wstring((int)s_panoHeight) +
           L" 米\\n";
    msg += L"面尺寸: " + std::to_wstring(fs) + L" × " +
           std::to_wstring(fs) + L"\\n";
    msg += L"文件: " + W(s_panoSavePath);
    MessageBoxW(G.hMain, msg.c_str(), L"全景导出完成",
                MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(
        G.hMain,
        L"全景图保存失败！\\n请检查 "
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
}"""

code = code[:pf_idx] + new_panopost + "\n" + code[pf_end:]
changes += 1
print("OK: replaced panoPostFrame with state machine version")

# ══════════════════════════════════════════════════════════════
# 6. Replace doPanoExport — use PANO_FACE_SIZE, update dialog info
# ══════════════════════════════════════════════════════════════

# Replace: s_panoFaceSize = std::min(G.vpW, G.vpH);
old_facesize = "s_panoFaceSize = std::min(G.vpW, G.vpH);"
new_facesize = "s_panoFaceSize = PANO_FACE_SIZE;"
if old_facesize in code:
    code = code.replace(old_facesize, new_facesize)
    changes += 1
    print("OK: patched s_panoFaceSize to PANO_FACE_SIZE")
else:
    print("WARN: s_panoFaceSize pattern not found")

# Replace the output size calculation
old_outsize = "int outW = s_panoFaceSize * 2, outH = s_panoFaceSize;"
new_outsize = "int outW = PANO_OUT_W, outH = PANO_OUT_H;"
if old_outsize in code:
    code = code.replace(old_outsize, new_outsize)
    changes += 1
    print("OK: patched output size to PANO_OUT_W/H")
else:
    print("WARN: outW/outH pattern not found (may already be fixed)")

print(f"\nTotal changes: {changes}")

if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print(f"OK: wrote {SRC}")
