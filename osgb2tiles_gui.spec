# -*- mode: python ; coding: utf-8 -*-
import sys, pathlib

# ── pyproj & PROJ data（七参数坐标转换，必选）────────────────────────
import pyproj as _pyproj
_pyproj_pkg = pathlib.Path(_pyproj.__file__).parent
_proj_data  = _pyproj_pkg / "proj_dir" / "share" / "proj"   # PROJ 数据库

a = Analysis(
    ['osgb2tiles_gui.py'],
    pathex=[],
    binaries=[],
    datas=[
        (str(_proj_data), 'pyproj/proj_dir/share/proj'),
    ],
    hiddenimports=[
        'pyproj',
        'pyproj.crs',
        'pyproj.crs.crs',
        'pyproj.transformer',
        'pyproj.datadir',
        'pyproj._compat',
        'pyproj.network',
        'certifi',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'matplotlib', 'numpy', 'pandas', 'PIL',
        'scipy', 'sklearn', 'torch', 'tensorflow',
        'IPython', 'jupyter',
    ],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='osgb2tiles_gui',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['osgb2tiles_gui.ico'],
)
