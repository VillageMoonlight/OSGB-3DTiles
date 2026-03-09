"""
GSD (Ground Sample Distance) + Auto Tiling patch.
Replaces pixel W/H dialog with GSD input (meters/pixel).
Auto-calculates output image size and splits into tiles if needed.
"""
import math

SRC = r"src\osgb_viewer.cpp"

with open(SRC, "rb") as f:
    raw = f.read()
code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

lines = code.split("\n")
print(f"Total lines: {len(lines)}")

# ═══════════════════════════════════════════════════════════════
# 1. Replace dialog section (find by marker)
# ═══════════════════════════════════════════════════════════════
dlg_start = -1
dlg_end = -1
for i, line in enumerate(lines):
    if "static int s_orthoW" in line:
        dlg_start = i - 1  # include the comment above
        break

# Find the end of showOrthoDlg function
for i in range(dlg_start, len(lines)):
    if "DialogBoxIndirectParamW" in lines[i]:
        # Find the closing brace of showOrthoDlg
        for j in range(i, len(lines)):
            if lines[j].strip() == "}":
                dlg_end = j
                break
        break

print(f"Dialog section: lines {dlg_start+1} to {dlg_end+1}")

NEW_DIALOG = r'''// \u2500\u2500 \u5730\u9762\u5206\u8fa8\u7387\u5bf9\u8bdd\u6846 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
static double s_orthoGSD = 0.5; // \u5730\u9762\u5206\u8fa8\u7387\uff08\u7c73/\u50cf\u7d20\uff09

static INT_PTR CALLBACK OrthoDlgProc(HWND hd, UINT msg, WPARAM wp, LPARAM) {
  switch (msg) {
  case WM_INITDIALOG:
    SetDlgItemTextW(hd, 101, L"0.5");
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(wp) == IDOK) {
      wchar_t buf[64] = {};
      GetDlgItemTextW(hd, 101, buf, 64);
      s_orthoGSD = _wtof(buf);
      if (s_orthoGSD < 0.001) s_orthoGSD = 0.001;
      if (s_orthoGSD > 1000.0) s_orthoGSD = 1000.0;
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

static bool showOrthoDlg() {
  alignas(4) BYTE buf[2048] = {};
  BYTE *p = buf;

  auto *dlg = (DLGTEMPLATE *)p;
  dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
  dlg->cdit = 4;
  dlg->cx = 210;
  dlg->cy = 60;
  p += sizeof(DLGTEMPLATE);
  *(WORD *)p = 0; p += 2; // menu
  *(WORD *)p = 0; p += 2; // class
  const wchar_t *ttl = L"\u6b63\u5c04\u8f93\u51fa - \u5730\u9762\u5206\u8fa8\u7387";
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
  addCtrl(SS_LEFT, 10, 14, 70, 10, (WORD)-1, 0x0082,
          L"\u5730\u9762\u5206\u8fa8\u7387(\u7c73/\u50cf\u7d20):");
  addCtrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 85, 12, 50, 14, 101, 0x0081,
          L"0.5");
  addCtrl(BS_DEFPUSHBUTTON | WS_TABSTOP, 40, 38, 55, 16, IDOK, 0x0080,
          L"\u786e\u5b9a\u5bfc\u51fa");
  addCtrl(WS_TABSTOP, 110, 38, 55, 16, IDCANCEL, 0x0080, L"\u53d6\u6d88");

  return DialogBoxIndirectParamW(G.hInst, (DLGTEMPLATE *)buf, G.hMain,
                                 OrthoDlgProc, 0) == IDOK;
}'''

lines[dlg_start:dlg_end+1] = NEW_DIALOG.split("\n")
print(f"[1] Replaced dialog section")

# ═══════════════════════════════════════════════════════════════
# 2. Replace state vars + orthoPreFrame + orthoPostFrame
# ═══════════════════════════════════════════════════════════════
# Find the export section start
export_start = -1
for i, line in enumerate(lines):
    if "static bool s_exporting" in line:
        export_start = i - 1  # include comment
        break

# Find doOrthoExport function start
do_export_start = -1
for i, line in enumerate(lines):
    if "static void doOrthoExport(bool nadir)" in line:
        do_export_start = i
        break

# Find doOrthoExport function end
brace = 0
do_export_end = -1
for i in range(do_export_start, len(lines)):
    brace += lines[i].count("{") - lines[i].count("}")
    if brace == 0 and i > do_export_start:
        do_export_end = i
        break

print(f"Export section: lines {export_start+1} to {do_export_end+1}")

NEW_EXPORT = r'''// \u2500\u2500 \u6b63\u5c04\u6e32\u67d3\u5e76\u5bfc\u51fa \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
static bool s_exporting = false;
static bool s_orthoActive = false;
static bool s_orthoNadir = false;
static int  s_orthoFrame = 0;
static int  s_orthoTotal = 30;
static double s_oL, s_oR, s_oB, s_oT, s_near, s_far;
static osg::Vec3d s_eye, s_at, s_up;
static osg::Matrixd s_savedProj;
static std::string s_savePath;

// \u5206\u5757\u72b6\u6001
static int    s_tileRow = 0, s_tileCol = 0;
static int    s_tilesX = 1,  s_tilesY = 1;
static int    s_totalW, s_totalH;
static double s_gsdWorld;         // \u4e16\u754c\u5750\u6807\u5355\u4f4d/\u50cf\u7d20
static double s_fullHalfW, s_fullHalfH; // \u573a\u666f\u534a\u5bbd\u534a\u9ad8
static osg::Vec3d s_sceneCenter;  // \u573a\u666f\u4e2d\u5fc3\u4e16\u754c\u5750\u6807
static std::string s_saveBase;    // \u6587\u4ef6\u540d\u4e0d\u542b\u6269\u5c55\u540d
static int s_tileSaved = 0;       // \u5df2\u4fdd\u5b58\u5757\u6570

// \u64cd\u7eb5\u5668\u72b6\u6001\u4fdd\u5b58
static osg::Vec3d s_savedManipCenter;
static double     s_savedManipDist;
static osg::Quat  s_savedManipRot;

// \u8ba1\u7b97\u5f53\u524d\u5206\u5757\u7684\u6295\u5f71\u8303\u56f4\uff08\u773c\u7a7a\u95f4\uff09
static void calcTileBounds() {
  double vpW = (double)G.vpW, vpH = (double)G.vpH;
  // \u6bcf\u5757\u8986\u76d6\u7684\u5730\u9762\u8303\u56f4
  double tileGndW = vpW * s_gsdWorld;
  double tileGndH = vpH * s_gsdWorld;
  s_oL = -s_fullHalfW + s_tileCol * tileGndW;
  s_oR = s_oL + tileGndW;
  s_oT = s_fullHalfH - s_tileRow * tileGndH;
  s_oB = s_oT - tileGndH;
  // \u6700\u540e\u4e00\u5217/\u884c\u53ef\u80fd\u8d85\u51fa\u573a\u666f\u8303\u56f4\uff0c\u4e0d\u505a\u622a\u65ad\uff08\u8fb9\u7f18\u5c31\u662f\u8fb9\u7f18\uff09
}

// \u5728\u4e3b\u5faa\u73af frame() \u4e4b\u524d\u8c03\u7528
static void orthoPreFrame() {
  if (!s_orthoActive) return;

  // \u8bbe\u7f6e\u5f53\u524d\u5206\u5757\u7684\u6b63\u5c04\u6295\u5f71
  G.viewer->getCamera()->setProjectionMatrixAsOrtho(
      s_oL, s_oR, s_oB, s_oT, s_near, s_far);

  // DOM \u6a21\u5f0f\uff1a\u8bbe\u7f6e\u64cd\u7eb5\u5668\u4fef\u89c6\u89d2
  if (s_orthoNadir && G.manip) {
    // \u64cd\u7eb5\u5668\u4e2d\u5fc3\u8ddf\u968f\u5f53\u524d\u5206\u5757\u4e2d\u5fc3
    double tileCX = s_sceneCenter.x() + (s_oL + s_oR) * 0.5;
    double tileCY = s_sceneCenter.y() + (s_oB + s_oT) * 0.5;
    G.manip->setCenter(osg::Vec3d(tileCX, tileCY, s_sceneCenter.z()));
    G.manip->setDistance(s_far * 0.45);
    G.manip->setRotation(osg::Quat()); // identity = \u4fef\u89c6
  }

  s_orthoFrame++;
  if (G.hStat && (s_orthoFrame % 5 == 0)) {
    int tileIdx = s_tileRow * s_tilesX + s_tileCol + 1;
    int tileTotal = s_tilesX * s_tilesY;
    std::wstring lb = s_orthoNadir ? L"\u6e32\u67d3DOM " : L"\u6e32\u67d3\u6b63\u4ea4 ";
    lb += L"[" + std::to_wstring(tileIdx) + L"/" + std::to_wstring(tileTotal) + L"] ";
    lb += std::to_wstring(s_orthoFrame) + L"/" + std::to_wstring(s_orthoTotal);
    SetWindowTextW(G.hStat, lb.c_str());
  }
}

// \u63a8\u8fdb\u5230\u4e0b\u4e00\u5757\uff0c\u8fd4\u56de true \u8868\u793a\u8fd8\u6709\u5757\u9700\u8981\u6e32\u67d3
static bool advanceTile() {
  s_tileCol++;
  if (s_tileCol >= s_tilesX) {
    s_tileCol = 0;
    s_tileRow++;
  }
  if (s_tileRow >= s_tilesY) return false;
  calcTileBounds();
  s_orthoFrame = 0;  // \u91cd\u7f6e\u5e27\u8ba1\u6570\u5668
  return true;
}

// \u5728\u4e3b\u5faa\u73af frame() \u4e4b\u540e\u8c03\u7528
static void orthoPostFrame() {
  if (!s_orthoActive) return;
  if (s_orthoFrame < s_orthoTotal) return;

  // \u5f53\u524d\u5757\u5df2\u6e32\u67d3\u8db3\u591f\u5e27 \u2192 \u622a\u56fe
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
  orthoPreFrame();
  G.viewer->frame();
  G.viewer->getCamera()->setFinalDrawCallback(nullptr);

  // \u8ba1\u7b97\u6587\u4ef6\u540d
  std::string tilePath;
  if (s_tilesX == 1 && s_tilesY == 1) {
    tilePath = s_saveBase + ".tif";
  } else {
    tilePath = s_saveBase + "_R" + std::to_string(s_tileRow)
               + "_C" + std::to_string(s_tileCol) + ".tif";
  }

  // \u5199 TIFF
  bool ok = writeTiff(tilePath, img->data(), outW, outH);
  if (ok) {
    auto stem = [](const std::string &p) {
      auto d = p.rfind('.');
      return d != std::string::npos ? p.substr(0, d) : p;
    };
    // TFW: \u5f53\u524d\u5757\u7684\u4e16\u754c\u5750\u6807\u5de6\u4e0a\u89d2
    double tileWorldL = s_sceneCenter.x() + s_oL;
    double tileWorldT = s_sceneCenter.y() + s_oT;
    writeTfw(stem(tilePath) + ".tfw", tileWorldL, tileWorldT,
             s_gsdWorld, s_gsdWorld);
    if (G.metaValid && !G.metaSrs.empty())
      writePrj(stem(tilePath) + ".prj", G.metaSrs);
    s_tileSaved++;
  }

  // \u63a8\u8fdb\u5230\u4e0b\u4e00\u5757
  if (advanceTile()) {
    // \u8fd8\u6709\u66f4\u591a\u5757\uff0c\u7ee7\u7eed\u6e32\u67d3
    return;
  }

  // \u5168\u90e8\u5b8c\u6210\uff0c\u6062\u590d\u76f8\u673a
  G.viewer->getCamera()->setProjectionMatrix(s_savedProj);
  if (s_orthoNadir && G.manip) {
    G.manip->setCenter(s_savedManipCenter);
    G.manip->setDistance(s_savedManipDist);
    G.manip->setRotation(s_savedManipRot);
  }
  s_orthoActive = false;
  s_exporting = false;

  // \u663e\u793a\u7ed3\u679c
  auto modeW = s_orthoNadir ? L"\u6b63\u5c04DOM" : L"\u6b63\u4ea4\u622a\u56fe";
  int totalTiles = s_tilesX * s_tilesY;
  if (G.hStat)
    SetWindowTextW(G.hStat, (std::wstring(modeW) + L" \u5bfc\u51fa\u5b8c\u6210: " +
                             std::to_wstring(s_tileSaved) + L" \u4e2a\u5206\u5757").c_str());

  std::wstring msg = std::wstring(modeW) + L"\u5df2\u6210\u529f\u5bfc\u51fa\uff01\n\n";
  msg += L"\u5730\u9762\u5206\u8fa8\u7387: " + std::to_wstring(s_orthoGSD) + L" \u7c73/\u50cf\u7d20\n";
  msg += L"\u603b\u50cf\u7d20: " + std::to_wstring(s_totalW) + L" \u00d7 " + std::to_wstring(s_totalH) + L"\n";
  if (totalTiles > 1)
    msg += L"\u5206\u5757: " + std::to_wstring(s_tilesX) + L" \u00d7 " + std::to_wstring(s_tilesY)
           + L" = " + std::to_wstring(totalTiles) + L" \u4e2a\n";
  msg += L"\u6587\u4ef6: " + W(s_saveBase) + L"*.tif";
  MessageBoxW(G.hMain, msg.c_str(), L"\u5bfc\u51fa\u5b8c\u6210", MB_OK | MB_ICONINFORMATION);

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

  // \u663e\u793a GSD \u5bf9\u8bdd\u6846
  if (!showOrthoDlg()) return;

  // \u8ba1\u7b97\u573a\u666f\u8303\u56f4
  double fovy, aspect, zNear, zFar;
  G.viewer->getCamera()->getProjectionMatrixAsPerspective(fovy, aspect, zNear, zFar);
  osg::Vec3d center = G.manip->getCenter();
  double dist = G.manip->getDistance();
  double halfH = dist * tan(osg::DegreesToRadians(fovy * 0.5));
  double halfW = halfH * aspect;

  // GSD \u6362\u7b97\uff1a\u5224\u65ad\u662f\u5426 WGS84
  bool isWGS84 = G.metaValid && (G.metaSrs.find("4326") != std::string::npos ||
                                  G.metaSrs.find("WGS") != std::string::npos ||
                                  G.metaSrs.find("wgs") != std::string::npos);
  if (isWGS84) {
    double lat = G.metaLat;
    double mPerDeg = 111320.0 * cos(lat * 3.14159265358979 / 180.0);
    s_gsdWorld = s_orthoGSD / mPerDeg;  // \u7c73 \u2192 \u5ea6
  } else {
    s_gsdWorld = s_orthoGSD;  // \u6295\u5f71\u5750\u6807\uff0c\u5355\u4f4d\u5df2\u662f\u7c73
  }

  // \u8ba1\u7b97\u603b\u8f93\u51fa\u5c3a\u5bf8
  double sceneW = halfW * 2.0, sceneH = halfH * 2.0;
  s_totalW = (int)ceil(sceneW / s_gsdWorld);
  s_totalH = (int)ceil(sceneH / s_gsdWorld);

  // \u8ba1\u7b97\u5206\u5757\u6570\uff08\u6bcf\u5757 = viewport \u5927\u5c0f\uff09
  s_tilesX = (int)ceil((double)s_totalW / G.vpW);
  s_tilesY = (int)ceil((double)s_totalH / G.vpH);

  // \u786e\u8ba4\u5bfc\u51fa\u4fe1\u606f
  std::wstring info = L"\u5730\u9762\u5206\u8fa8\u7387: " + std::to_wstring(s_orthoGSD) + L" \u7c73/\u50cf\u7d20\n";
  if (isWGS84)
    info += L"WGS84 \u50cf\u7d20\u8ddd: " + std::to_wstring(s_gsdWorld) + L" \u5ea6\n";
  info += L"\u603b\u50cf\u7d20: " + std::to_wstring(s_totalW) + L" \u00d7 " + std::to_wstring(s_totalH) + L"\n";
  info += L"\u5206\u5757: " + std::to_wstring(s_tilesX) + L" \u00d7 " + std::to_wstring(s_tilesY);
  info += L" (\u6bcf\u5757 " + std::to_wstring(G.vpW) + L"\u00d7" + std::to_wstring(G.vpH) + L")\n\n";
  info += L"\u662f\u5426\u7ee7\u7eed\uff1f";
  if (MessageBoxW(G.hMain, info.c_str(), L"\u786e\u8ba4\u5bfc\u51fa",
                  MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
    return;

  // \u9009\u62e9\u4fdd\u5b58\u8def\u5f84
  std::string savePath = pickSaveTif();
  if (savePath.empty()) return;
  // \u63d0\u53d6\u4e0d\u542b\u6269\u5c55\u540d\u7684\u57fa\u7840\u540d
  auto dotPos = savePath.rfind('.');
  s_saveBase = (dotPos != std::string::npos) ? savePath.substr(0, dotPos) : savePath;

  // \u521d\u59cb\u5316\u5bfc\u51fa\u72b6\u6001
  s_exporting = true;
  s_orthoActive = true;
  s_orthoNadir = nadir;
  s_orthoFrame = 0;
  s_tileRow = 0;
  s_tileCol = 0;
  s_tileSaved = 0;

  s_savedProj = G.viewer->getCamera()->getProjectionMatrix();
  if (G.manip) {
    s_savedManipCenter = G.manip->getCenter();
    s_savedManipDist   = G.manip->getDistance();
    s_savedManipRot    = G.manip->getRotation();
  }

  double sceneH2 = bs.radius() * 4.0;
  s_sceneCenter = center;
  s_fullHalfW = halfW;
  s_fullHalfH = halfH;
  s_near = 0.1;
  s_far = sceneH2 * 2.0;
  s_eye = osg::Vec3d(center.x(), center.y(), center.z() + sceneH2 * 0.9);
  s_at  = osg::Vec3d(center.x(), center.y(), center.z());
  s_up  = osg::Vec3d(0, 1, 0);

  // \u8ba1\u7b97\u7b2c\u4e00\u5757\u7684\u6295\u5f71\u8303\u56f4
  calcTileBounds();

  auto label = nadir ? L"\u6b63\u5728\u6e32\u67d3\u6b63\u5c04DOM..."
                     : L"\u6b63\u5728\u6e32\u67d3\u6b63\u4ea4\u622a\u56fe...";
  if (G.hStat) SetWindowTextW(G.hStat, label);
}'''

lines[export_start:do_export_end+1] = NEW_EXPORT.split("\n")
print(f"[2] Replaced export section (state vars + pre/post frame + doOrthoExport)")

# ═══════════════════════════════════════════════════════════════
# 3. Remove reference to old s_worldL, s_worldT, s_rW, s_rH
# ═══════════════════════════════════════════════════════════════
# These were removed from the state vars. Check if they're used elsewhere.
# (They shouldn't be since they were only used in the old orthoPostFrame)

code = "\n".join(lines)
if has_crlf:
    code = code.replace("\n", "\r\n")

with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("\nDone! Changes:")
print("  - Dialog: GSD input (meters/pixel) instead of W/H")
print("  - Auto tile splitting based on viewport size")
print("  - WGS84 auto-detection and degree conversion")
print("  - Per-tile TIF/TFW/PRJ output with _R{row}_C{col} naming")
print("  - Confirmation dialog showing total size and tile count")
