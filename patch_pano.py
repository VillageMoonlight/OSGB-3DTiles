"""
Panorama Export: CubeMap 6-face rendering + equirectangular projection → JPG.
"""
import re

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()
code = raw.decode("utf-8")
lines = code.split("\n")
print(f"Total lines: {len(lines)}")

changed = False

# ═══════════════════════════════════════════════════════════════
# 1. Add #include <cmath> and <osgDB/WriteFile> if missing
# ═══════════════════════════════════════════════════════════════
if "<cmath>" not in code:
    for i, line in enumerate(lines):
        if "#include <algorithm>" in line:
            lines.insert(i, "#include <cmath>")
            print("[1a] Added #include <cmath>")
            changed = True
            break

# Re-join since indices may have shifted
code = "\n".join(lines)
lines = code.split("\n")

if "osgDB/WriteFile" not in code:
    for i, line in enumerate(lines):
        if "osgDB/ReadFile" in line:
            lines.insert(i + 1, "#include <osgDB/WriteFile>")
            print("[1b] Added #include <osgDB/WriteFile>")
            changed = True
            break

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# 2. Add #define ID_PANO_EXPORT 1501
# ═══════════════════════════════════════════════════════════════
if "ID_PANO_EXPORT" not in code:
    for i, line in enumerate(lines):
        if "ID_ORTHO_DOM" in line and "#define" in line:
            lines.insert(i + 1, "#define ID_PANO_EXPORT 1501")
            print("[2] Added ID_PANO_EXPORT")
            changed = True
            break

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# 3. Insert panorama code after doOrthoExport function
# ═══════════════════════════════════════════════════════════════
# Find "static void loadOsgb" as the insertion anchor
insert_before = -1
for i, line in enumerate(lines):
    if "static void loadOsgb" in line:
        insert_before = i
        break

if insert_before < 0:
    print("ERROR: could not find loadOsgb anchor")
    exit(1)

PANO_CODE = r'''
// \u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550 \u5168\u666f\u8f93\u51fa \u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550

// CubeMap \u516d\u9762\u65b9\u5411\u5b9a\u4e49 (Z-up)
struct CubeFaceDir { osg::Vec3d fwd, right, up; };
static const CubeFaceDir s_cubeDirs[6] = {
  {{0,1,0},  {1,0,0},   {0,0,1}},   // 0 Front  +Y
  {{0,-1,0}, {-1,0,0},  {0,0,1}},   // 1 Back   -Y
  {{1,0,0},  {0,-1,0},  {0,0,1}},   // 2 Right  +X
  {{-1,0,0}, {0,1,0},   {0,0,1}},   // 3 Left   -X
  {{0,0,1},  {1,0,0},   {0,-1,0}},  // 4 Top    +Z
  {{0,0,-1}, {1,0,0},   {0,1,0}},   // 5 Bottom -Z
};

static bool   s_panoActive = false;
static int    s_panoFace = 0;      // 0..5
static int    s_panoFrame = 0;
static int    s_panoTotal = 30;
static double s_panoHeight = 50.0;
static double s_panoFar = 10000.0;
static int    s_panoFaceSize = 0;
static osg::Vec3d s_panoEye;
static osg::Matrixd s_panoProjSaved;
static osg::ref_ptr<osg::Image> s_panoFaces[6];
static std::string s_panoSavePath;

// \u64cd\u7eb5\u5668\u72b6\u6001\u4fdd\u5b58\uff08\u5168\u666f\u7528\uff09
static osg::Vec3d s_panoSavedCenter;
static double     s_panoSavedDist;
static osg::Quat  s_panoSavedRot;

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
      if (s_panoHeight < 0.1) s_panoHeight = 0.1;
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
  *(WORD *)p = 0; p += 2;
  *(WORD *)p = 0; p += 2;
  const wchar_t *ttl = L"\u5168\u666f\u8f93\u51fa - \u89c6\u70b9\u9ad8\u5ea6";
  size_t tl = (wcslen(ttl) + 1) * 2;
  memcpy(p, ttl, tl);
  p += tl;

  auto addCtrl = [&](DWORD st, short x, short y, short cx, short cy, WORD id,
                     WORD cls, const wchar_t *txt) {
    p = (BYTE *)(((uintptr_t)p + 3) & ~3);
    auto *it = (DLGITEMTEMPLATE *)p;
    it->style = st | WS_CHILD | WS_VISIBLE;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p += sizeof(DLGITEMTEMPLATE);
    *(WORD *)p = 0xFFFF; p += 2;
    *(WORD *)p = cls;    p += 2;
    size_t n = (wcslen(txt) + 1) * 2;
    memcpy(p, txt, n);
    p += n;
    *(WORD *)p = 0; p += 2;
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

// CubeMap \u2192 \u7b49\u8ddd\u77e9\u5f62\u6295\u5f71
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
      if (ay >= ax && ay >= az) face = (dy > 0) ? 0 : 1;
      else if (ax >= az)        face = (dx > 0) ? 2 : 3;
      else                      face = (dz > 0) ? 4 : 5;

      const auto &fd = s_cubeDirs[face];
      double fwdDot = dx * fd.fwd.x() + dy * fd.fwd.y() + dz * fd.fwd.z();
      double u = (dx * fd.right.x() + dy * fd.right.y() + dz * fd.right.z()) / fwdDot;
      double v = (dx * fd.up.x()    + dy * fd.up.y()    + dz * fd.up.z())    / fwdDot;

      // [-1,1] \u2192 \u50cf\u7d20 (osg::Image bottom-up, v=-1=row0=bottom)
      int fx = std::clamp((int)((u + 1.0) * 0.5 * (fs - 1) + 0.5), 0, fs - 1);
      int fy = std::clamp((int)((v + 1.0) * 0.5 * (fs - 1) + 0.5), 0, fs - 1);

      const unsigned char *src = s_panoFaces[face]->data() + (fy * fs + fx) * 3;
      // \u8f93\u51fa: py=0 \u662f\u5929\u9876 \u2192 osg::Image \u91cc\u662f\u6700\u540e\u4e00\u884c
      unsigned char *d = dst + ((outH - 1 - py) * outW + px) * 3;
      d[0] = src[0]; d[1] = src[1]; d[2] = src[2];
    }
  }
  return eq;
}

// \u5168\u666f pre/post frame
static void panoPreFrame() {
  if (!s_panoActive) return;

  // \u8bbe\u7f6e 90\u00b0 FOV 1:1 \u900f\u89c6\u6295\u5f71
  G.viewer->getCamera()->setProjectionMatrixAsPerspective(
      90.0, 1.0, 0.1, s_panoFar);

  // \u8bbe\u7f6e\u5f53\u524d\u9762\u7684\u89c6\u89d2\u65b9\u5411
  const auto &fd = s_cubeDirs[s_panoFace];
  osg::Vec3d target = s_panoEye + fd.fwd * 100.0;
  G.viewer->getCamera()->setViewMatrixAsLookAt(s_panoEye, target, fd.up);

  s_panoFrame++;
  if (G.hStat && (s_panoFrame % 5 == 0)) {
    static const wchar_t *faceNames[] = {
      L"Front(+Y)", L"Back(-Y)", L"Right(+X)",
      L"Left(-X)", L"Top(+Z)", L"Bottom(-Z)"
    };
    std::wstring lb = L"\u5168\u666f\u6e32\u67d3 [" +
        std::to_wstring(s_panoFace + 1) + L"/6] " +
        faceNames[s_panoFace] + L" " +
        std::to_wstring(s_panoFrame);
    SetWindowTextW(G.hStat, lb.c_str());
  }
}

static void panoPostFrame() {
  if (!s_panoActive) return;

  // \u81ea\u9002\u5e94\u7b49\u5f85\uff1a\u81f3\u5c11 s_panoTotal \u5e27 + DatabasePager \u7a7a\u95f2
  if (s_panoFrame < s_panoTotal) return;
  osgDB::DatabasePager *pager = G.viewer->getDatabasePager();
  if (pager && s_panoFrame < 300) {
    int pending = pager->getFileRequestListSize();
    if (pending > 0) return;
    if (s_panoFrame < s_panoTotal + 10) return;
  }

  // \u622a\u56fe\u5f53\u524d\u9762
  int fs = s_panoFaceSize;
  int ox = (G.vpW - fs) / 2;
  int oy = (G.vpH - fs) / 2;
  s_panoFaces[s_panoFace] = new osg::Image();
  s_panoFaces[s_panoFace]->allocateImage(fs, fs, 1, GL_RGB, GL_UNSIGNED_BYTE);

  struct ReadCB : public osg::Camera::DrawCallback {
    osg::Image *dst; int x0, y0, sz;
    ReadCB(osg::Image *d, int x, int y, int s) : dst(d), x0(x), y0(y), sz(s) {}
    void operator()(osg::RenderInfo &) const override {
      dst->readPixels(x0, y0, sz, sz, GL_RGB, GL_UNSIGNED_BYTE);
    }
  };
  G.viewer->getCamera()->setFinalDrawCallback(
      new ReadCB(s_panoFaces[s_panoFace].get(), ox, oy, fs));
  panoPreFrame();
  G.viewer->frame();
  G.viewer->getCamera()->setFinalDrawCallback(nullptr);

  // \u63a8\u8fdb\u5230\u4e0b\u4e00\u9762
  s_panoFace++;
  if (s_panoFace < 6) {
    s_panoFrame = 0;
    return;
  }

  // \u5168\u90e8 6 \u9762\u5b8c\u6210 \u2192 \u62fc\u63a5
  if (G.hStat)
    SetWindowTextW(G.hStat, L"\u6b63\u5728\u62fc\u63a5\u5168\u666f\u56fe...");

  int outW = fs * 2, outH = fs;
  auto equirect = cubemapToEquirect(outW, outH);

  // \u4fdd\u5b58 JPG
  bool ok = osgDB::writeImageFile(*equirect, s_panoSavePath);

  // \u6062\u590d\u76f8\u673a
  G.viewer->getCamera()->setProjectionMatrix(s_panoProjSaved);
  G.viewer->setCameraManipulator(G.manip, false);
  if (G.manip) {
    G.manip->setCenter(s_panoSavedCenter);
    G.manip->setDistance(s_panoSavedDist);
    G.manip->setRotation(s_panoSavedRot);
  }
  s_panoActive = false;
  s_exporting = false;

  // \u91ca\u653e\u9762\u56fe\u50cf
  for (int i = 0; i < 6; i++) s_panoFaces[i] = nullptr;

  if (ok) {
    std::wstring msg = L"\u5168\u666f\u56fe\u5df2\u6210\u529f\u5bfc\u51fa\uff01\n\n";
    msg += L"\u5206\u8fa8\u7387: " + std::to_wstring(outW) + L" \u00d7 " + std::to_wstring(outH) + L"\n";
    msg += L"\u89c6\u70b9\u9ad8\u5ea6: " + std::to_wstring((int)s_panoHeight) + L" \u7c73\n";
    msg += L"\u6587\u4ef6: " + W(s_panoSavePath);
    MessageBoxW(G.hMain, msg.c_str(), L"\u5168\u666f\u5bfc\u51fa\u5b8c\u6210",
                MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(G.hMain, L"\u5168\u666f\u56fe\u4fdd\u5b58\u5931\u8d25\uff01\n\u8bf7\u68c0\u67e5 OSG jpeg \u63d2\u4ef6\u662f\u5426\u53ef\u7528\u3002",
                L"\u9519\u8bef", MB_OK | MB_ICONERROR);
  }
  if (G.hStat) SetWindowTextW(G.hStat, L"\u5c31\u7eea");
}

static void doPanoExport() {
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

  if (!showPanoDlg()) return;

  // \u9009\u62e9\u4fdd\u5b58\u8def\u5f84
  std::string savePath = pickSaveJpg();
  if (savePath.empty()) return;

  // \u8ba1\u7b97\u89c6\u70b9\u4f4d\u7f6e\uff1a\u5f53\u524d\u5c4f\u5e55\u4e2d\u5fc3 + \u6307\u5b9a\u9ad8\u5ea6
  osg::Vec3d center = G.manip->getCenter();
  s_panoEye = osg::Vec3d(center.x(), center.y(), center.z() + s_panoHeight);
  s_panoFar = bs.radius() * 4.0;
  s_panoFaceSize = std::min(G.vpW, G.vpH);
  s_panoSavePath = savePath;

  // \u4fdd\u5b58\u72b6\u6001
  s_panoProjSaved = G.viewer->getCamera()->getProjectionMatrix();
  if (G.manip) {
    s_panoSavedCenter = G.manip->getCenter();
    s_panoSavedDist   = G.manip->getDistance();
    s_panoSavedRot    = G.manip->getRotation();
  }

  // \u79fb\u9664\u64cd\u7eb5\u5668\uff0c\u76f4\u63a5\u63a7\u5236\u76f8\u673a
  G.viewer->setCameraManipulator(nullptr, false);

  // \u786e\u8ba4
  int outW = s_panoFaceSize * 2, outH = s_panoFaceSize;
  std::wstring info = L"\u89c6\u70b9\u9ad8\u5ea6: " + std::to_wstring((int)s_panoHeight) + L" \u7c73\n";
  info += L"\u5168\u666f\u5206\u8fa8\u7387: " + std::to_wstring(outW) + L" \u00d7 " + std::to_wstring(outH) + L"\n";
  info += L"\u9700\u6e32\u67d3 6 \u4e2a\u7acb\u65b9\u4f53\u9762\n\n\u662f\u5426\u7ee7\u7eed\uff1f";
  if (MessageBoxW(G.hMain, info.c_str(), L"\u786e\u8ba4\u5168\u666f\u5bfc\u51fa",
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
'''

lines.insert(insert_before, PANO_CODE)
print(f"[3] Inserted panorama code before loadOsgb at line {insert_before+1}")

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# 4. Add toolbar button
# ═══════════════════════════════════════════════════════════════
for i, line in enumerate(lines):
    if "ID_ORTHO_DOM" in line and "DOM" in line and "L\"" in line:
        lines.insert(i + 1, '      {ID_PANO_EXPORT, L"\\U0001f310 \\u5168\\u666f", nullptr},')
        print(f"[4] Added toolbar button after line {i+1}")
        break

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# 5. Add menu item in buildMenu
# ═══════════════════════════════════════════════════════════════
for i, line in enumerate(lines):
    if "ID_ORTHO_EXPORT" in line and "AppendMenuW" in line:
        # Insert after the ortho export menu item
        indent = "  "
        new_items = [
            indent + 'AppendMenuW(mT, MF_STRING, ID_ORTHO_DOM,',
            indent + '            L"DOM\\u8f93\\u51fa(&D)...");',
            indent + 'AppendMenuW(mT, MF_SEPARATOR, 0, nullptr);',
            indent + 'AppendMenuW(mT, MF_STRING, ID_PANO_EXPORT,',
            indent + '            L"\\u5168\\u666f\\u8f93\\u51fa(&P)...");',
        ]
        # Find the end of the ortho_export entry (next line)
        j = i + 1
        while j < len(lines) and lines[j].strip().startswith('L"'):
            j += 1
        for k, new_line in enumerate(new_items):
            lines.insert(j + k, new_line)
        print(f"[5] Added menu items after line {j}")
        break

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# 6. Add WM_COMMAND handler
# ═══════════════════════════════════════════════════════════════
for i, line in enumerate(lines):
    if "case ID_ORTHO_DOM:" in line:
        # Find the break after this case
        for j in range(i, min(i + 5, len(lines))):
            if "break;" in lines[j]:
                new_case = [
                    "    case ID_PANO_EXPORT:",
                    "      doPanoExport();",
                    "      break;"
                ]
                for k, nl in enumerate(new_case):
                    lines.insert(j + 1 + k, nl)
                print(f"[6] Added WM_COMMAND handler after line {j+1}")
                break
        break

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# 7. Add panoPreFrame/panoPostFrame to main loop
# ═══════════════════════════════════════════════════════════════
for i, line in enumerate(lines):
    if "orthoPreFrame();" in line and "viewer" not in line:
        lines.insert(i + 1, "      panoPreFrame();")
        print(f"[7a] Added panoPreFrame() after line {i+1}")
        break

code = "\n".join(lines)
lines = code.split("\n")

for i, line in enumerate(lines):
    if "orthoPostFrame();" in line:
        lines.insert(i + 1, "      panoPostFrame();")
        print(f"[7b] Added panoPostFrame() after line {i+1}")
        break

code = "\n".join(lines)
lines = code.split("\n")

# ═══════════════════════════════════════════════════════════════
# Write output
# ═══════════════════════════════════════════════════════════════
code = "\n".join(lines)

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print(f"\nDone! New total lines: {len(lines)}")
print("Features added:")
print("  - ID_PANO_EXPORT = 1501")
print("  - Toolbar button: 🌐 全景")
print("  - Menu: 工具 > 全景输出")
print("  - Height input dialog")
print("  - CubeMap 6-face rendering with adaptive LOD waiting")
print("  - Equirectangular projection (2:1 panorama)")
print("  - JPG save via osgDB::writeImageFile")
