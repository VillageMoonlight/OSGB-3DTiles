#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""osgb2tiles_gui.py - OSGB→3D Tiles GUI v2.2"""
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import json, os, sys, threading, subprocess, queue, tempfile, re, math, webbrowser
import xml.etree.ElementTree as ET
import http.server, socketserver
from pathlib import Path

APP_TITLE = "OSGB → 3D Tiles 转换工具  v2.2"
CONFIG_FILE = "osgb2tiles_config.json"
_HERE = Path(sys.executable).parent if getattr(sys,'frozen',False) else Path(__file__).parent
# exe 优先在 build\bin 目录（调试模式），其次在 GUI 同目录（发布模式）
_EXE_BUILD = _HERE / "build" / "bin" / "osgb2tiles.exe"
_EXE_LOCAL = _HERE / "osgb2tiles.exe"
EXE = str(_EXE_BUILD) if _EXE_BUILD.exists() else str(_EXE_LOCAL)
ICON  = str(_HERE / "osgb2tiles_gui.ico")

BG="#1e1e2e"; BG2="#181825"; SURF="#313244"; OVL="#45475a"
FG="#cdd6f4"; FGS="#a6adc8"; FGM="#585b70"
BLU="#89b4fa"; GRN="#a6e3a1"; RED="#f38ba8"; YLW="#f9e2af"; SKY="#89dceb"; TEA="#94e2d5"

def _gauss_inv(x,y,z=0):
    a=6378137.;f=1/298.257222101;e2=2*f-f*f;ep2=e2/(1-e2)
    east,north=x,y
    if y>x and y>1e7: east,north=y,x
    zone=int(east//1e6)
    if 13<=zone<=45: CM=zone*3.; east-=zone*1e6
    else: return None,None,z
    N0=north;E0=east-5e5
    A0=1-e2/4-3*e2**2/64-5*e2**3/256;A2=3/8*(e2+e2**2/4+15*e2**3/128)
    A4=15/256*(e2**2+3*e2**3/4);A6=35*e2**3/3072
    phi=N0/a
    for _ in range(10):
        M=a*(A0*phi-A2*math.sin(2*phi)+A4*math.sin(4*phi)-A6*math.sin(6*phi))
        dM=a*(A0-2*A2*math.cos(2*phi)+4*A4*math.cos(4*phi)-6*A6*math.cos(6*phi))
        phi+=(N0-M)/dM
    sn=math.sin(phi);cn=math.cos(phi);tn=sn/cn
    N=a/math.sqrt(1-e2*sn*sn);T=tn*tn;C=ep2*cn*cn
    R=a*(1-e2)/(1-e2*sn*sn)**1.5;D=E0/N
    lat=phi-(N*tn/R)*(D**2/2-(5+3*T+10*C-4*C*C-9*ep2)*D**4/24+(61+90*T+298*C+45*T*T-252*ep2-3*C*C)*D**6/720)
    lon=math.radians(CM)+(D-(1+2*T+C)*D**3/6+(5-2*C+28*T-3*C*C+8*ep2+24*T*T)*D**5/120)/cn
    return math.degrees(lon),math.degrees(lat),z

def try_meta(d):
    for fp in [str(Path(d)/f) for f in ("metadata.xml","production_meta.xml","doc.xml","Data/metadata.xml")]:
        if not os.path.exists(fp): continue
        try:
            root=ET.parse(fp).getroot()
            se=root.find(".//SRS"); oe=root.find(".//SRSOrigin")
            if se is None or oe is None: continue
            srs=(se.text or "").strip()
            pts=[(v.strip()) for v in (oe.text or "0,0,0").split(",")]
            x,y=float(pts[0]),float(pts[1]); z=float(pts[2]) if len(pts)>2 else 0.
            m=re.search(r'EPSG:(\d+)',srs); epsg=int(m.group(1)) if m else 0
            if not srs or epsg==4326 or 'WGS' in srs.upper():
                if -180<=x<=180 and -90<=y<=90:
                    return dict(lon=x,lat=y,h=z,srs=srs,epsg=epsg,rx=x,ry=y,src=os.path.basename(fp),mth="WGS84-直接")
            lo,la,z2=_gauss_inv(x,y,z)
            if lo: return dict(lon=lo,lat=la,h=z2,srs=srs,epsg=epsg,rx=x,ry=y,src=os.path.basename(fp),mth="高斯逆算(仅预览)")
        except: pass
    return None

def L(p,t,fg=None,sz=9,**kw): return tk.Label(p,text=t,fg=fg or FGS,bg=p.cget("bg"),font=("Microsoft YaHei UI",sz),**kw)
def E(p,v,w=20,**kw): return tk.Entry(p,textvariable=v,width=w,bg=SURF,fg=FG,insertbackground=FG,relief="flat",font=("Consolas",10),highlightthickness=1,highlightcolor=BLU,highlightbackground=OVL,**kw)
def C(p,t,v,**kw): return tk.Checkbutton(p,text=t,variable=v,bg=p.cget("bg"),fg=FG,selectcolor=SURF,activebackground=p.cget("bg"),font=("Microsoft YaHei UI",9),**kw)
def B(p,t,c,w=10,bg=OVL,fg=FG,**kw):
    b=tk.Button(p,text=t,command=c,width=w,bg=bg,fg=fg,activebackground=BLU,activeforeground=BG2,relief="flat",font=("Microsoft YaHei UI",9),cursor="hand2",**kw)
    b.bind("<Enter>",lambda e:b.configure(bg=BLU,fg=BG2)); b.bind("<Leave>",lambda e:b.configure(bg=bg,fg=fg)); return b
def SEC(p,t,ic=""):
    f=tk.Frame(p,bg=BG2); tk.Label(f,text=f" {ic}  {t} ",fg=SKY,bg=BG2,font=("Microsoft YaHei UI",10,"bold")).pack(side="left")
    tk.Frame(f,bg=FGM,height=1).pack(side="left",fill="x",expand=True,padx=4,pady=10); return f
def CB(p,v,vals,w=8): return ttk.Combobox(p,textvariable=v,values=vals,width=w,state="readonly")

_srv=None; _srv_port=0
_data_srv=None; _data_port=0

class _CORSHandler(http.server.SimpleHTTPRequestHandler):
    """带 CORS 的静态文件处理器，允许跨端口加载 B3DM/KTX2 资源"""
    def log_message(self,*a): pass
    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin','*')
        self.send_header('Access-Control-Allow-Methods','GET,OPTIONS')
        super().end_headers()
    def do_OPTIONS(self):
        self.send_response(204); self.end_headers()

def start_srv(folder):
    """启动/重启静态资源服务器（服务 folder 目录）"""
    global _srv,_srv_port
    if _srv:
        try: _srv.shutdown()
        except: pass
        _srv=None
    os.chdir(folder)
    _srv=socketserver.TCPServer(("127.0.0.1",0),_CORSHandler)
    _srv.allow_reuse_address=True
    _srv_port=_srv.server_address[1]
    threading.Thread(target=_srv.serve_forever,daemon=True).start()
    return _srv_port

def start_data_srv(folder):
    """启动/重启数据服务器（服务 3D Tiles 输出目录）"""
    global _data_srv,_data_port
    if _data_srv:
        try: _data_srv.shutdown()
        except: pass
        _data_srv=None

    class H(_CORSHandler):
        def __init__(self,*a,**kw):
            super().__init__(*a, directory=folder, **kw)

    _data_srv=socketserver.TCPServer(("127.0.0.1",0),H)
    _data_srv.allow_reuse_address=True
    _data_port=_data_srv.server_address[1]
    threading.Thread(target=_data_srv.serve_forever,daemon=True).start()
    return _data_port


OSGB_HTML=r"""<!DOCTYPE html><html><head><meta charset="utf-8"><style>body{margin:0;background:#0d1117;overflow:hidden}#info{position:fixed;top:8px;left:50%;transform:translateX(-50%);color:#89b4fa;font:12px/1.5 monospace;background:rgba(0,0,0,.6);padding:4px 12px;border-radius:4px;pointer-events:none}</style></head><body><div id="info">🏔 OSGB 根节点预览（仅粗模，用于确认位置外观）| 鼠标左键旋转 | 滚轮缩放 | 右键平移</div>
<script type="importmap">{"imports":{"three":"https://cdn.jsdelivr.net/npm/three@0.165/build/three.module.js","three/addons/":"https://cdn.jsdelivr.net/npm/three@0.165/examples/jsm/"}}</script>
<script type="module">
import*as T from'three';import{OrbitControls}from'three/addons/controls/OrbitControls.js';import{GLTFLoader}from'three/addons/loaders/GLTFLoader.js';
const r=new T.WebGLRenderer({antialias:true});r.setSize(innerWidth,innerHeight);r.setPixelRatio(devicePixelRatio);r.outputColorSpace=T.SRGBColorSpace;document.body.appendChild(r.domElement);
const s=new T.Scene();s.background=new T.Color(0x0d1117);
const c=new T.PerspectiveCamera(45,innerWidth/innerHeight,.1,1e7);
const oc=new OrbitControls(c,r.domElement);oc.enableDamping=true;oc.dampingFactor=.05;
s.add(new T.AmbientLight(0xffffff,1.5));const dl=new T.DirectionalLight(0xffffff,2);dl.position.set(1,2,1);s.add(dl);
const url=new URLSearchParams(location.search).get('u');
if(url){new GLTFLoader().load(url,g=>{const m=g.scene;const box=new T.Box3().setFromObject(m);const ctr=box.getCenter(new T.Vector3());m.position.sub(ctr);s.add(m);const sz=box.getSize(new T.Vector3()).length();c.position.set(0,sz*.4,sz*1.2);oc.update();document.getElementById('info').textContent='✅ 加载完成 | 左键旋转 | 滚轮缩放 | 右键平移';},xhr=>{document.getElementById('info').textContent='⏳ 加载中... '+Math.round(xhr.loaded/xhr.total*100)+'%';},e=>document.getElementById('info').textContent='❌ 加载失败: '+e);}
window.addEventListener('resize',()=>{c.aspect=innerWidth/innerHeight;c.updateProjectionMatrix();r.setSize(innerWidth,innerHeight);});
(function a(){requestAnimationFrame(a);oc.update();r.render(s,c);})();
</script></body></html>"""

TILES_HTML=r"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>3D Tiles Preview</title>
<script>window.CESIUM_BASE_URL='/cesium/';</script>
<script src="/cesium/Cesium.js"></script>
<link href="/cesium/Widgets/widgets.css" rel="stylesheet">
<style>
html,body,#c{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:#0d1117}
#tip{position:fixed;top:8px;left:50%;transform:translateX(-50%);color:#89b4fa;
  font:12px monospace;background:rgba(0,0,0,.75);padding:4px 14px;border-radius:6px;z-index:999;pointer-events:none}
#panel{position:fixed;top:8px;right:8px;background:rgba(20,20,30,.88);color:#cdd6f4;
  font:12px/1.6 monospace;padding:10px 14px;border-radius:8px;z-index:999;min-width:210px}
#panel h3{margin:0 0 6px;font-size:13px;color:#89dceb;border-bottom:1px solid #313244;padding-bottom:4px}
.row{display:flex;align-items:center;justify-content:space-between;gap:6px;margin:3px 0}
.row label{flex:1;color:#a6adc8;font-size:11px;white-space:nowrap}
.row input[type=range]{width:80px}
.row span{width:32px;text-align:right;color:#f9e2af;font-weight:bold;font-size:11px}
#stats{margin-top:6px;padding-top:5px;border-top:1px solid #313244;font-size:11px;color:#a6adc8;line-height:1.7}
/* 完全屏蔽 Cesium 底部版权栏和 Ion logo */
.cesium-viewer-bottom,.cesium-credit-logoContainer,
.cesium-credit-textContainer,.cesium-viewer-toolbar{display:none!important}
</style></head><body>
<div id="c"></div><div id="tip">🌍 正在加载 3D Tiles...</div>
<div id="panel">
  <h3>⚡ 渲染性能控制</h3>
  <div class="row"><label>瓦片误差(SSE)</label><input type="range" id="sse" min="1" max="64" step="1" value="8"><span id="sseVal">8</span></div>
  <div class="row"><label>最大内存(MB)</label><input type="range" id="mem" min="256" max="4096" step="128" value="1024"><span id="memVal">1024</span></div>
  <div class="row"><label>最大请求数</label><input type="range" id="req" min="4" max="64" step="4" value="18"><span id="reqVal">18</span></div>
  <div class="row"><label>全局球误差</label><input type="range" id="gsse" min="1" max="16" step="1" value="4"><span id="gsseVal">4</span></div>
  <div id="stats">📊 等待加载...</div>
</div>
<script>
Cesium.Ion.defaultAccessToken='';
const v=new Cesium.Viewer('c',{
  baseLayerPicker:false,geocoder:false,homeButton:true,
  sceneModePicker:false,navigationHelpButton:false,
  animation:false,timeline:false,fullscreenButton:true,
  imageryProvider:false,   /* 不加载外网底图 */
  terrainProvider:new Cesium.EllipsoidTerrainProvider(),  /* 用椭球面代替地形服务 */
  creditContainer:document.createElement('div')  /* 隐藏版权容器 */
});
v.scene.globe.enableLighting=false;
v.scene.globe.baseColor=new Cesium.Color(0.06,0.06,0.1,1.0);  /* 深色底色，无底图 */
v.scene.globe.depthTestAgainstTerrain=false;  /* 关键：禁止地形深度测试，否则建筑被地球剪裁 */
v.scene.globe.maximumScreenSpaceError=4;
v.scene.skyBox.show=false;
v.scene.sun.show=false;
v.scene.moon.show=false;
v.scene.skyAtmosphere.show=false;
v.scene.backgroundColor=new Cesium.Color(0.05,0.05,0.08,1.0);
Cesium.RequestScheduler.maximumRequestsPerServer=18;
const tip=document.getElementById('tip'),stats=document.getElementById('stats');
let ts=null;
function bind(id,vid,fn){const i=document.getElementById(id),s=document.getElementById(vid);
  s.textContent=i.value;i.addEventListener('input',()=>{s.textContent=i.value;fn(Number(i.value));});}
bind('sse','sseVal',val=>{if(ts)ts.maximumScreenSpaceError=val;});
bind('mem','memVal',val=>{if(ts)ts.maximumMemoryUsage=val;});
bind('req','reqVal',val=>{Cesium.RequestScheduler.maximumRequestsPerServer=val;});
bind('gsse','gsseVal',val=>{v.scene.globe.maximumScreenSpaceError=val;});
const url=new URLSearchParams(location.search).get('u');
if(url){(async()=>{try{
  ts=await Cesium.Cesium3DTileset.fromUrl(url,{
    maximumScreenSpaceError:8,maximumMemoryUsage:1024,
    skipLevelOfDetail:false,cullRequestsWhileMoving:true,
    cullRequestsWhileMovingMultiplier:10,foveatedScreenSpaceError:true,
    foveatedTimeDelay:0.2,loadSiblings:false,progressiveResolutionHeightFraction:0.3});
  ts.maximumScreenSpaceError=Number(document.getElementById('sse').value);
  ts.maximumMemoryUsage=Number(document.getElementById('mem').value);
  v.scene.primitives.add(ts);await v.zoomTo(ts);
  tip.style.color='#a6e3a1';tip.textContent='✅ 3D Tiles 加载完成';
  setTimeout(()=>tip.style.display='none',3000);
  v.scene.postRender.addEventListener(()=>{if(!ts)return;
    const s=ts.statistics;if(!s)return;
    stats.innerHTML=`📦 渲染: <b>${s.numberOfFeaturesRendered??'–'}</b> tiles<br>`+
      `📥 加载中: <b>${s.numberOfPendingRequests??'–'}</b><br>`+
      `💾 内存: <b>${Math.round((ts.totalMemoryUsageInBytes??0)/1048576)}</b> MB`;});
}catch(e){tip.style.color='#f38ba8';tip.textContent='❌ 加载失败: '+(e.message||e);}
})();}else{tip.textContent='❌ 未提供 tileset URL';}
</script></body></html>"""

class App(tk.Tk):
    def __init__(self):
        super().__init__(); self.title(APP_TITLE); self.configure(bg=BG)
        self.resizable(True,True)
        self.state('zoomed')  # 最大化，兼容 DPI 缩放
        if os.path.isfile(ICON):
            try: self.iconbitmap(ICON)
            except: pass
        self._q=queue.Queue(); self._running=False; self._proc=None
        self._init_vars(); self._build_ui(); self._load_config(); self._poll()
        self.minsize(900,640)
        # 窗口首次显示（Map）后设置分隔条，此时 winfo_width 已有真实值
        self.bind("<Map>", self._on_map_sash)

    def _init_vars(self):
        self.v_in=tk.StringVar(); self.v_out=tk.StringVar()
        self.v_lon=tk.StringVar(value="0.0"); self.v_lat=tk.StringVar(value="0.0"); self.v_alt=tk.StringVar(value="0.0")
        self.v_srs=tk.StringVar(value="选择 OSGB 目录后自动读取")
        self.v_7p=tk.BooleanVar(value=False)
        self.v_dx,self.v_dy,self.v_dz=[tk.StringVar(value="0.0") for _ in range(3)]
        self.v_rx,self.v_ry,self.v_rz=[tk.StringVar(value="0.0") for _ in range(3)]
        self.v_sc=tk.StringVar(value="0.0")
        self.v_7p_result=tk.StringVar(value="")
        self.v_fmt=tk.StringVar(value="b3dm"); self.v_thr=tk.StringVar(value="4")
        self.v_sim=tk.BooleanVar(value=False); self.v_srt=tk.DoubleVar(value=0.5)
        self.v_tex=tk.StringVar(value="ktx2"); self.v_tsz=tk.StringVar(value="2048")
        self.v_jq=tk.StringVar(value="85"); self.v_wq=tk.StringVar(value="80"); self.v_wl=tk.BooleanVar(value=False)
        # KTX2 sub-options
        self.v_ktx2_mode=tk.StringVar(value="etc1s")    # etc1s / uastc
        self.v_ktx2_quality=tk.IntVar(value=2)           # 1-5
        self.v_vrb=tk.BooleanVar(value=False)
        self.v_draco=tk.BooleanVar(value=False)
        self.v_draco_bits=tk.StringVar(value="14")
        self.v_geo_err=tk.DoubleVar(value=0.5)
        self.v_cfg=tk.StringVar(value=str(Path.cwd()/CONFIG_FILE))
        # 七参数纠正后锁定的坐标（None表示未锁定）
        self._corr_lon=None; self._corr_lat=None; self._corr_alt=None
        self._tiles_preview_url=None  # 缓存已挂载目录的预览URL

    def _build_ui(self):
        hdr=tk.Frame(self,bg=BG2); hdr.pack(fill="x")
        hi=tk.Frame(hdr,bg=BG2); hi.pack(fill="x",padx=12,pady=7)
        tk.Label(hi,text="⬡",fg=SKY,bg=BG2,font=("Segoe UI Emoji",16)).pack(side="left")
        tk.Label(hi,text="  OSGB → 3D Tiles 转换工具",fg=FG,bg=BG2,font=("Microsoft YaHei UI",14,"bold")).pack(side="left")
        tk.Label(hi,text="  v2.2  |  GDAL/PROJ · JPEG/PNG/WebP/KTX2· 支持七参数精准坐标转换 · 三维预览",fg=FGM,bg=BG2,font=("Microsoft YaHei UI",9)).pack(side="left",pady=4)
        B(hi,"📂 打开输出目录",self._open_out,w=13).pack(side="right",padx=4)

        body=tk.PanedWindow(self,orient="horizontal",bg=BG,sashwidth=6,sashrelief="flat")
        body.pack(fill="both",expand=True)
        self._body=body

        # ── 左：双Tab参数区（主体1/2）──
        lf=tk.Frame(body,bg=BG); body.add(lf,minsize=420)
        self._lf=lf  # 保留引用以便后续 paneconfigure
        self._tabs=ttk.Notebook(lf); self._tabs.pack(fill="both",expand=True,padx=4,pady=4)

        # Tab1：转换任务
        t1=tk.Frame(self._tabs,bg=BG); self._tabs.add(t1,text="  ⚙ 转换任务  ")
        lp=tk.Frame(t1,bg=BG); lp.pack(fill="both",expand=True,padx=10,pady=6)

        # § 路径配置（带预览按钮）
        SEC(lp,"路径配置","📁").pack(fill="x",pady=(0,2))
        pf=tk.Frame(lp,bg=BG2,padx=10,pady=8); pf.pack(fill="x")
        self._prow_preview(pf,"OSGB 输入目录:",self.v_in,self._browse_in,
                           "🏔 OSGB预览",self._preview_osgb,"_btn_osgb_prev")
        self._prow_preview(pf,"3DTiles 输出目录:",self.v_out,self._browse_out,
                           "🌍 预览",self._preview_tiles,"_btn_tiles_prev")
        self._btn_tiles_prev.configure(state="disabled")

        # § 坐标原点
        SEC(lp,"地理坐标原点（WGS84）","🌐").pack(fill="x",pady=(8,2))
        cf=tk.Frame(lp,bg=BG2,padx=10,pady=8); cf.pack(fill="x")
        sr=tk.Frame(cf,bg=BG2); sr.pack(fill="x",pady=(0,5))
        L(sr,"坐标系:").pack(side="left")
        tk.Label(sr,textvariable=self.v_srs,fg=GRN,bg=BG2,font=("Microsoft YaHei UI",9),wraplength=560,justify="left").pack(side="left",padx=4)
        cr=tk.Frame(cf,bg=BG2); cr.pack(fill="x")
        for lb,var,tip in [("经度 lon (°)",self.v_lon,"原点十进制度"),("纬度 lat (°)",self.v_lat,"原点十进制度"),("高程 alt (m)",self.v_alt,"WGS84 椭球高")]:
            blk=tk.Frame(cr,bg=BG2); blk.pack(side="left",padx=(0,24))
            L(blk,lb,fg=BLU).pack(anchor="w"); E(blk,var,w=15).pack(anchor="w"); L(blk,tip,sz=8).pack(anchor="w")

        # § 七参数
        SEC(lp,"七参数坐标转换（可选）","🔄").pack(fill="x",pady=(8,2))
        tf=tk.Frame(lp,bg=BG2,padx=10,pady=6); tf.pack(fill="x")
        th=tk.Frame(tf,bg=BG2); th.pack(fill="x",pady=(0,2))
        C(th,"启用 Bursa-Wolf 七参数转换",self.v_7p,command=self._tog7p).pack(side="left")
        # 详细内容框：未勾选时整体隐藏
        self._f7_detail=tk.Frame(tf,bg=BG2)  # 不 pack，由 _tog7p 控制
        tk.Label(self._f7_detail,text="转换方向：原始 OSGB 地方坐标 → WGS84（3DTiles 目标坐标系）\n仅当 OSGB 数据以地方独立坐标系存储、且已知精确转换参数时启用",
                 fg=YLW,bg=BG2,font=("Microsoft YaHei UI",8),justify="left").pack(anchor="w",pady=(0,4))
        self._f7=tk.Frame(self._f7_detail,bg=SURF,padx=8,pady=6); self._f7.pack(fill="x")
        p7=[("ΔX (m)",self.v_dx),("ΔY (m)",self.v_dy),("ΔZ (m)",self.v_dz),
            ("Rx (\")",self.v_rx),("Ry (\")",self.v_ry),("Rz (\")",self.v_rz),("m (ppm)",self.v_sc)]
        tips=["X轴平移","Y轴平移","Z轴平移","X轴旋转(角秒)","Y轴旋转(角秒)","Z轴旋转(角秒)","比例因子(ppm)"]
        for i,((lb,var),tip) in enumerate(zip(p7,tips)):
            blk=tk.Frame(self._f7,bg=SURF); blk.grid(row=i//4,column=i%4,padx=(0,10),pady=(0,4),sticky="w")
            L(blk,lb,fg=BLU).pack(anchor="w"); E(blk,var,w=11).pack(anchor="w"); L(blk,tip,sz=7).pack(anchor="w")
        # 更新按钮放在第1行第3列（m(ppm)同行右侧）
        btn_blk=tk.Frame(self._f7,bg=SURF); btn_blk.grid(row=1,column=3,padx=(0,4),pady=(0,4),sticky="sw")
        L(btn_blk," ",sz=9).pack(anchor="w")  # 占位对齐
        self._btn_7p_apply=B(btn_blk,"🔄 更新坐标",self._apply_7p_update,w=10,bg=OVL); 
        self._btn_7p_apply.pack(anchor="w")
        self._btn_7p_apply.configure(state="disabled")
        # 纠正后坐标显示行
        rr=tk.Frame(self._f7_detail,bg=BG2); rr.pack(fill="x",pady=(4,0))
        tk.Label(rr,textvariable=self.v_7p_result,fg=GRN,bg=BG2,font=("Microsoft YaHei UI",9)).pack(side="left")
        # 参数变化时清除已锁定的纠正坐标（提示用户重新点更新）
        for v in [self.v_dx,self.v_dy,self.v_dz,self.v_rx,self.v_ry,self.v_rz,self.v_sc]:
            v.trace_add("write", self._on_7p_param_change)
        self._tog7p()

        # ─ Tab2：转换选项 ─
        t2=tk.Frame(self._tabs,bg=BG); self._tabs.add(t2,text="  🛠 转换选项  ")
        lp2=tk.Frame(t2,bg=BG); lp2.pack(fill="both",expand=True,padx=10,pady=6)
        lp=lp2  # 后续 of/r1..r6/配置文件 都挂到 lp2

        # § 输出格式
        SEC(lp,"输出格式 & 线程","📄").pack(fill="x",pady=(0,2))
        of=tk.Frame(lp,bg=BG2,padx=10,pady=8); of.pack(fill="x")
        # 行1：格式/线程/日志
        r1=tk.Frame(of,bg=BG2); r1.pack(fill="x",pady=(0,6))
        L(r1,"输出格式:",fg=BLU).pack(side="left")
        for f in ("b3dm","glb"):
            tk.Radiobutton(r1,text=f,variable=self.v_fmt,value=f,bg=BG2,fg=FG,selectcolor=SURF,activebackground=BG2,font=("Microsoft YaHei UI",9)).pack(side="left",padx=5)
        L(r1,"   线程数:",fg=BLU).pack(side="left")
        CB(r1,self.v_thr,["1","2","4","8","12","16"],w=4).pack(side="left",padx=4)
        C(r1,"  详细日志",self.v_vrb).pack(side="left",padx=12)
        # 行2：网格简化
        r2=tk.Frame(of,bg=BG2); r2.pack(fill="x",pady=(0,6))
        C(r2,"启用网格简化",self.v_sim).pack(side="left")
        L(r2,"  简化比例:",fg=BLU).pack(side="left")
        sl=ttk.Scale(r2,from_=0.1,to=1.0,orient="horizontal",variable=self.v_srt,length=180); sl.pack(side="left",padx=4)
        self._sll=tk.Label(r2,text="0.50",fg=TEA,bg=BG2,font=("Consolas",9),width=4); self._sll.pack(side="left")
        sl.configure(command=lambda v:self._sll.configure(text=f"{float(v):.2f}"))
        # 行3：纹理格式选择
        r3=tk.Frame(of,bg=BG2); r3.pack(fill="x",pady=(0,4))
        L(r3,"纹理格式:",fg=BLU).pack(side="left")
        CB(r3,self.v_tex,["ktx2","jpg","png","webp"],w=7).pack(side="left",padx=4)
        L(r3,"  最大尺寸:",fg=BLU).pack(side="left")
        CB(r3,self.v_tsz,["512","1024","2048","4096"],w=6).pack(side="left",padx=4)
        L(r3,"px",fg=FGM).pack(side="left")
        # 行4：纹理参数（动态）
        self._rtex=tk.Frame(of,bg=BG2); self._rtex.pack(fill="x",pady=(0,2))
        self.v_tex.trace_add("write",lambda *_:self._upd_tex())
        self._upd_tex()
        # 行5：Draco 几何压缩
        r5=tk.Frame(of,bg=BG2); r5.pack(fill="x",pady=(0,2))
        C(r5,"启用 Draco 几何压缩（体积可减~70%）",self.v_draco).pack(side="left")
        L(r5,"  量化位数:",fg=BLU).pack(side="left")
        CB(r5,self.v_draco_bits,["8","10","12","14","16"],w=4).pack(side="left",padx=4)
        L(r5,"bit",fg=FGM).pack(side="left")
        # 行6：几何误差系数
        r6=tk.Frame(of,bg=BG2); r6.pack(fill="x",pady=(0,4))
        L(r6,"LOD 误差系数:",fg=BLU).pack(side="left")
        sl2=ttk.Scale(r6,from_=0.1,to=2.0,orient="horizontal",variable=self.v_geo_err,length=140); sl2.pack(side="left",padx=4)
        self._gell=tk.Label(r6,text="0.50",fg=TEA,bg=BG2,font=("Consolas",9),width=4); self._gell.pack(side="left")
        sl2.configure(command=lambda v:self._gell.configure(text=f"{float(v):.2f}"))
        L(r6," (越大加载越激进)",fg=FGM).pack(side="left")

        # § 配置文件
        SEC(lp,"配置文件","💾").pack(fill="x",pady=(8,2))
        gf=tk.Frame(lp,bg=BG2,padx=10,pady=8); gf.pack(fill="x")
        gr=tk.Frame(gf,bg=BG2); gr.pack(fill="x")
        E(gr,self.v_cfg,w=52).pack(side="left",padx=(0,6))
        B(gr,"保存",self._save_cfg,w=6).pack(side="left",padx=2)
        B(gr,"加载",self._load_cfg_dlg,w=6).pack(side="left")

        # ── 右：转换日志（1/2）──
        rf=tk.Frame(body,bg=BG2); body.add(rf,minsize=260)
        lh=tk.Frame(rf,bg=BG2); lh.pack(fill="x",padx=8,pady=(8,2))
        L(lh,"📋 转换日志",fg=SKY,sz=10).pack(side="left")
        B(lh,"清空",self._clear_log,w=5,bg=SURF).pack(side="right")
        self._log=scrolledtext.ScrolledText(rf,bg=BG2,fg=FG,font=("Consolas",9),
            wrap="word",state="disabled",relief="flat",padx=6,pady=4)
        self._log.pack(fill="both",expand=True,padx=4,pady=4)
        for tag,fg_c in [("ok",GRN),("err",RED),("info",BLU),("warn",YLW),("dim",FGM),("hd",SKY)]:
            self._log.tag_configure(tag,foreground=fg_c)
        # 保留兼容字段（_log2/_log3 指向同一 log）
        self._log2=self._log; self._log3=self._log
        self._nb=self._tabs  # 兼容旧引用

        # 底部工具栏
        bot=tk.Frame(self,bg=BG2,pady=7); bot.pack(fill="x",side="bottom")
        bl=tk.Frame(bot,bg=BG2); bl.pack(side="left",fill="x",expand=True,padx=12)
        self._prog=ttk.Progressbar(bl,mode="determinate",maximum=100,length=100)
        self._prog.pack(fill="x",expand=True)
        self._stat=L(bl,"就绪",fg=FGM); self._stat.pack(anchor="w",pady=(2,0))
        br=tk.Frame(bot,bg=BG2); br.pack(side="right",padx=10)
        B(br,"⬛ 停止",self._stop,w=8,bg="#5c2828",fg=RED).pack(side="left",padx=4)
        B(br,"▶ 开始转换",self._start,w=14,bg="#1a4d36",fg=GRN).pack(side="left",padx=4)

        s=ttk.Style(self); s.theme_use("clam")
        s.configure("Horizontal.TProgressbar",troughcolor=SURF,background=GRN,lightcolor=GRN,darkcolor=GRN,bordercolor=BG2,thickness=8)
        s.configure("TCombobox",fieldbackground=SURF,background=OVL,foreground=FG,arrowcolor=BLU)
        s.map("TCombobox",fieldbackground=[("readonly",SURF)])
        s.configure("TScale",background=BG2,troughcolor=SURF)
        s.configure("TNotebook",background=BG,borderwidth=0)
        s.configure("TNotebook.Tab",background=SURF,foreground=FGS,font=("Microsoft YaHei UI",9),padding=[8,4])
        s.map("TNotebook.Tab",background=[("selected",BG2)],foreground=[("selected",SKY)])

    def _on_map_sash(self, event=None):
        """窗口首次显示后解绑，延迟200ms让布局稳定后再设分隔条。"""
        self.unbind("<Map>")
        self.after(200, self._set_sash)

    def _set_sash(self):
        """用 paneconfigure 直接设置左侧面板宽度为 body 的50%。"""
        bw = self._body.winfo_width()
        rw = self.winfo_width()
        # 写调试文件
        try:
            import pathlib
            debug_f = pathlib.Path(__file__).parent / "sash_debug.txt"
            with open(debug_f, "a", encoding="utf-8") as f:
                f.write(f"_set_sash: body.winfo_width={bw}, root.winfo_width={rw}\n")
        except:
            pass
        # body 或 root 任一宽度合法则设置
        w = bw if bw >= 400 else rw
        if w < 400:
            self.after(100, self._set_sash)  # 未就绪，100ms 后重试
            return
        half = w // 2
        try:
            self._body.paneconfigure(self._lf, width=half)
        except:
            try:
                self._body.sashpos(0, half)
            except:
                pass

    def _prow(self,p,label,var,cmd):
        row=tk.Frame(p,bg=BG2); row.pack(fill="x",pady=3)
        tk.Label(row,text=label,fg=BLU,bg=BG2,width=16,anchor="e",font=("Microsoft YaHei UI",9)).pack(side="left")
        E(row,var,w=40).pack(side="left",padx=(6,4))
        B(row,"📂 选择",cmd,w=8).pack(side="left")

    def _prow_preview(self,p,label,var,cmd,prev_text,prev_cmd,btn_attr):
        row=tk.Frame(p,bg=BG2); row.pack(fill="x",pady=3)
        tk.Label(row,text=label,fg=BLU,bg=BG2,width=16,anchor="e",font=("Microsoft YaHei UI",9)).pack(side="left")
        E(row,var,w=36).pack(side="left",padx=(6,4))
        B(row,"📂 选择",cmd,w=8).pack(side="left",padx=(0,4))
        btn=B(row,prev_text,prev_cmd,w=10,bg=SURF); btn.pack(side="left")
        setattr(self,btn_attr,btn)

    def _upd_tex(self):
        for w in self._rtex.winfo_children(): w.destroy()
        fmt=self.v_tex.get()
        if fmt=="ktx2":
            # 行一：ETC1S / UASTC 模式
            a=tk.Frame(self._rtex,bg=BG2); a.pack(fill="x",pady=(0,2))
            L(a,"KTX2 压缩模式:",fg=BLU).pack(side="left")
            CB(a,self.v_ktx2_mode,["etc1s","uastc"],w=8).pack(side="left",padx=4)
            L(a,"(OSGB倒影采样=ETC1S，BIM精模=UASTC)",fg=FGM).pack(side="left")
            # 行二：压缩级别
            b=tk.Frame(self._rtex,bg=BG2); b.pack(fill="x",pady=(0,2))
            L(b,"压缩质量:",fg=BLU).pack(side="left")
            sl3=ttk.Scale(b,from_=1,to=5,orient="horizontal",variable=self.v_ktx2_quality,length=120); sl3.pack(side="left",padx=4)
            self._kqll=tk.Label(b,text=str(self.v_ktx2_quality.get()),fg=TEA,bg=BG2,font=("Consolas",9),width=2); self._kqll.pack(side="left")
            sl3.configure(command=lambda v:self._kqll.configure(text=str(int(float(v)))))
            L(b,"[1-5]（越大压缩越慢）",fg=FGM).pack(side="left")

        elif fmt=="jpg":
            L(self._rtex,"JPEG 品质:",fg=BLU).pack(side="left")
            E(self._rtex,self.v_jq,w=4).pack(side="left",padx=(4,2))
            L(self._rtex,"[1-100]",fg=FGM).pack(side="left")
        elif fmt=="png":
            L(self._rtex,"PNG 无损压缩，体积较大，无需额外参数",fg=FGM).pack(side="left")
        elif fmt=="webp":
            C(self._rtex,"无损模式",self.v_wl).pack(side="left",padx=(0,8))
            L(self._rtex,"Lossy 品质:",fg=BLU).pack(side="left")
            E(self._rtex,self.v_wq,w=4).pack(side="left",padx=(4,2))
            L(self._rtex,"[1-100]（无损时忽略）",fg=FGM).pack(side="left")

    def _tog7p(self, *_):
        enabled = self.v_7p.get()
        if enabled:
            self._f7_detail.pack(fill="x", pady=(2,0))
            self._btn_7p_apply.configure(state="normal")
        else:
            self._f7_detail.pack_forget()
            self._btn_7p_apply.configure(state="disabled")
            # 取消七参数时清除锁定坐标
            self._corr_lon=None; self._corr_lat=None; self._corr_alt=None
            self.v_7p_result.set("")

    def _on_7p_param_change(self, *_):
        """七参数输入变化时，清除已锁定的纠正坐标，提示用户重新点更新。"""
        if self._corr_lon is not None:
            self._corr_lon=None; self._corr_lat=None; self._corr_alt=None
            self.v_7p_result.set("⚠ 参数已变更，请重新点击【更新坐标】")

    def _apply_7p_update(self):
        """点击【更新坐标】：用七参数计算纠正后的WGS84坐标并锁定。"""
        try:
            lon=float(self.v_lon.get() or 0)
            lat=float(self.v_lat.get() or 0)
            alt=float(self.v_alt.get() or 0)
        except:
            messagebox.showerror("错误","请先填写地理坐标原点（经纬度/高程）")
            return
        r=self._calc7p(lon, lat, alt)
        if r[0] is None:
            messagebox.showerror("错误","七参数计算失败，请检查 pyproj 是否正常安装")
            return
        self._corr_lon, self._corr_lat, self._corr_alt = r
        self.v_7p_result.set(
            f"✅ 已锁定 WGS84 坐标：lon={r[0]:.6f}°  lat={r[1]:.6f}°  alt={r[2]:.1f}m\n"
            f"   （原始输入：lon={lon:.6f}°  lat={lat:.6f}°  alt={alt:.1f}m）")
        self._log_msg(
            f"[7P] 七参数纠正完成：{lon:.6f}°,{lat:.6f}° → {r[0]:.6f}°,{r[1]:.6f}° alt:{r[2]:.1f}m\n","ok")

    def _upd_7p_result(self, *_):
        """兼容旧逻辑（不再自动触发，仅供显式调用）。"""
        if not self.v_7p.get():
            self.v_7p_result.set("")

    def _browse_in(self):
        d=filedialog.askdirectory(title="选择 OSGB 输入目录")
        if d:
            self.v_in.set(d); self._auto_coord(d)
            self._btn_osgb_prev.configure(state="normal")

    def _browse_out(self):
        d=filedialog.askdirectory(title="选择 3DTiles 输出目录")
        if d:
            self.v_out.set(d)
            self._mount_tiles_dir(d)

    def _mount_tiles_dir(self, out_dir):
        """后台启动/重启数据服务，挂载 out_dir 为 3DTiles 根，更新预览按钮状态。"""
        if not out_dir or not os.path.isdir(out_dir):
            self._tiles_preview_url = None
            self._btn_tiles_prev.configure(state="disabled")
            return
        # 有 tileset.json 才启用按钮
        has_ts = os.path.exists(os.path.join(out_dir, "tileset.json"))
        self._btn_tiles_prev.configure(state="normal" if has_ts else "disabled")
        self._log_msg(f"[INFO] 挂载 3DTiles 目录: {out_dir}\n", "info")

        def _do():
            try:
                stat_port = start_srv(str(_HERE))
                html_p = str(_HERE / "viewer.html")
                with open(html_p, "w", encoding="utf-8") as f:
                    f.write(TILES_HTML)
                data_port = start_data_srv(out_dir)
                tiles_url = f"http://127.0.0.1:{data_port}/tileset.json"
                import time as _time
                ts_stamp = int(_time.time())
                url = f"http://127.0.0.1:{stat_port}/viewer.html?t={ts_stamp}&u={tiles_url}"
                self._tiles_preview_url = url
                self._q.put(("tiles_mounted", url, has_ts))
            except Exception as e:
                self._q.put(("log", f"[ERR] 挂载数据服务失败: {e}\n", "err"))

        threading.Thread(target=_do, daemon=True).start()

    def _open_out(self):
        d=self.v_out.get()
        if d and os.path.isdir(d): os.startfile(d)
        else: messagebox.showwarning("提示","输出目录不存在")

    def _clear_log(self):
        self._log.configure(state="normal"); self._log.delete("1.0","end"); self._log.configure(state="disabled")

    def _auto_coord(self,d):
        self.v_srs.set("正在读取元数据…"); self.update_idletasks()
        meta=try_meta(d)
        if meta:
            self.v_lon.set(f"{meta['lon']:.6f}"); self.v_lat.set(f"{meta['lat']:.6f}"); self.v_alt.set(f"{meta['h']:.3f}")
            self.v_srs.set(f"✔ EPSG:{meta['epsg']}  方式:{meta['mth']}  来源:{meta['src']}  原始:X={meta['rx']:.2f} Y={meta['ry']:.2f}")
            self._log_msg(f"[AUTO] lon={meta['lon']:.6f}° lat={meta['lat']:.6f}° alt={meta['h']:.1f}m\n","ok")
        else:
            self.v_srs.set("⚠ 未找到元数据，请手动输入坐标")
            self._log_msg("[WARN] 未能自动读取坐标，请手动填写\n","warn")

    def _build_cfg(self):
        cfg={"inputPath":self.v_in.get(),"outputPath":self.v_out.get(),
             "longitude":float(self.v_lon.get() or 0),"latitude":float(self.v_lat.get() or 0),"height":float(self.v_alt.get() or 0),
             "tileFormat":self.v_fmt.get(),"threads":int(self.v_thr.get() or 4),
             "simplifyMesh":self.v_sim.get(),"simplifyRatio":round(self.v_srt.get(),2),
             "textureFormat":self.v_tex.get(),"compressTexture":self.v_tex.get()!="jpg",
             "ktx2Mode":self.v_ktx2_mode.get(),
             "ktx2Quality":self.v_ktx2_quality.get(),
             "maxTextureSize":int(self.v_tsz.get() or 2048),
             "jpegQuality":int(self.v_jq.get() or 85),
             "webpQuality":int(self.v_wq.get() or 80),"webpLossless":self.v_wl.get(),
             "verbose":self.v_vrb.get(),
             "compressGeometry":self.v_draco.get(),
             "dracoQuantBits":int(self.v_draco_bits.get() or 14),
             "geometricErrorScale":round(self.v_geo_err.get(),2),
             "mergeLevel":-1}  # -1=自动（由CLI根据瓦片数量智能计算）
        if self.v_7p.get():
            cfg["transform7p"]={"enabled":True,
                "dx":float(self.v_dx.get() or 0),"dy":float(self.v_dy.get() or 0),"dz":float(self.v_dz.get() or 0),
                "rx":float(self.v_rx.get() or 0),"ry":float(self.v_ry.get() or 0),"rz":float(self.v_rz.get() or 0),
                "scale":float(self.v_sc.get() or 0)}
        return cfg


    def _eff_coords(self):
        """返回实际传给主程序的 (lon, lat, alt)：
        若七参数已点击更新并锁定，返回锁定的纠正坐标；
        否则返回原始坐标。"""
        lon=float(self.v_lon.get() or 0)
        lat=float(self.v_lat.get() or 0)
        alt=float(self.v_alt.get() or 0)
        if self.v_7p.get() and self._corr_lon is not None:
            return self._corr_lon, self._corr_lat, self._corr_alt
        return lon, lat, alt

    def _calc7p(self, lon, lat, alt):
        """用 pyproj 做 Bursa-Wolf 七参数转换，返回 (lon84, lat84, alt)"""
        try:
            from pyproj import CRS, Transformer
            dx=float(self.v_dx.get() or 0); dy=float(self.v_dy.get() or 0)
            dz=float(self.v_dz.get() or 0); rx=float(self.v_rx.get() or 0)
            ry=float(self.v_ry.get() or 0); rz=float(self.v_rz.get() or 0)
            sc=float(self.v_sc.get() or 0)
            p4=f"+proj=longlat +ellps=GRS80 +towgs84={dx},{dy},{dz},{rx},{ry},{rz},{sc} +no_defs"
            t=Transformer.from_crs(CRS.from_proj4(p4), CRS.from_epsg(4326), always_xy=True)
            lon84,lat84=t.transform(lon, lat)
            return lon84, lat84, alt
        except Exception as e:
            return None, None, None

    def _save_cfg(self):
        try:
            p=self.v_cfg.get()
            with open(p,"w",encoding="utf-8") as f: json.dump(self._build_cfg(),f,ensure_ascii=False,indent=2)
            self._log_msg(f"[INFO] 配置已保存: {p}\n","info"); self._set_stat("配置已保存 ✔")
        except Exception as e: messagebox.showerror("保存失败",str(e))

    def _load_config(self):
        p=str(Path.cwd()/CONFIG_FILE)
        if os.path.exists(p): self._load_from(p)

    def _load_cfg_dlg(self):
        f=filedialog.askopenfilename(title="选择配置文件",filetypes=[("JSON","*.json"),("所有","*")])
        if f: self._load_from(f)

    def _load_from(self,path):
        try:
            with open(path,encoding="utf-8") as f: c=json.load(f)
            self.v_in.set(c.get("inputPath","")); self.v_out.set(c.get("outputPath",""))
            self.v_lon.set(str(c.get("longitude",0))); self.v_lat.set(str(c.get("latitude",0))); self.v_alt.set(str(c.get("height",0)))
            self.v_fmt.set(c.get("tileFormat","b3dm")); self.v_thr.set(str(c.get("threads",4)))
            self.v_sim.set(c.get("simplifyMesh",False)); self.v_srt.set(c.get("simplifyRatio",0.5))
            self.v_tex.set(c.get("textureFormat","ktx2")); self.v_tsz.set(str(c.get("maxTextureSize",2048)))
            self.v_ktx2_mode.set(c.get("ktx2Mode","etc1s"))
            self.v_ktx2_quality.set(c.get("ktx2Quality",2))
            self.v_jq.set(str(c.get("jpegQuality",85))); self.v_wq.set(str(c.get("webpQuality",80))); self.v_wl.set(c.get("webpLossless",False))
            self.v_vrb.set(c.get("verbose",False))
            self.v_draco.set(c.get("compressGeometry",False))
            self.v_draco_bits.set(str(c.get("dracoQuantBits",14)))
            self.v_geo_err.set(c.get("geometricErrorScale",0.5))
            self._gell.configure(text=f"{self.v_geo_err.get():.2f}")
            self._upd_tex()  # refresh KTX2 sub-panel
            if "transform7p" in c:
                t=c["transform7p"]; self.v_7p.set(bool(t.get("enabled",False)))
                for k,v in [("dx",self.v_dx),("dy",self.v_dy),("dz",self.v_dz),("rx",self.v_rx),("ry",self.v_ry),("rz",self.v_rz),("scale",self.v_sc)]:
                    v.set(str(t.get(k,0)))
                self._tog7p()
            self.v_cfg.set(path); self._log_msg(f"[INFO] 配置已加载: {path}\n","info")
            # 自动挂载输出目录（恢复配置时立即启动数据服务）
            out = c.get("outputPath","")
            if out and os.path.isdir(out):
                self._tiles_preview_url = None
                self._mount_tiles_dir(out)
        except Exception as e: self._log_msg(f"[ERR] 加载配置失败: {e}\n","err")

    def _start(self):
        if self._running: messagebox.showinfo("提示","转换进行中…"); return
        if not self.v_in.get(): messagebox.showerror("错误","请选择 OSGB 输入目录"); return
        if not self.v_out.get(): messagebox.showerror("错误","请选择输出目录"); return
        if not os.path.isfile(EXE):
            messagebox.showerror("错误",f"找不到 osgb2tiles.exe:\n{EXE}\n请将 exe 与 GUI 放在同一目录"); return
        self._save_cfg()
        cfg=self._build_cfg()
        lon,lat,alt = self._eff_coords()
        cmd=[EXE,"-i",cfg["inputPath"],"-o",cfg["outputPath"],
             "--lon",str(lon),"--lat",str(lat),"--alt",str(alt),
             "--format",cfg["tileFormat"],"--threads",str(cfg["threads"]),
             "--simplify-ratio",str(cfg["simplifyRatio"]),
             "--tex-format",cfg["textureFormat"],
             "--tex-size",str(cfg["maxTextureSize"]),
             "--jpeg-quality",str(cfg["jpegQuality"]),
             "--webp-quality",str(cfg["webpQuality"])]
        if cfg["simplifyMesh"]: cmd.append("--simplify")
        if cfg["compressTexture"]: cmd.append("--compress-tex")
        if cfg.get("webpLossless"): cmd.append("--webp-lossless")
        if cfg.get("compressGeometry"):
            cmd.extend(["--draco","--draco-bits",str(cfg.get("dracoQuantBits",14))])
        if cfg.get("textureFormat") == "ktx2":
            cmd.extend(["--ktx2-mode", cfg.get("ktx2Mode","etc1s"),
                        "--ktx2-quality", str(cfg.get("ktx2Quality",2))])
        if cfg.get("geometricErrorScale",0.5) != 0.5:
            cmd.extend(["--geo-error",str(cfg.get("geometricErrorScale",0.5))])
        # --merge-level: -1=自动（让CLI自行计算），0=强制不合并，N=强制N层
        cmd.extend(["--merge-level", str(cfg.get("mergeLevel", -1))])
        cmd.append("--verbose")  # 始终开启详细日志（调试阶段）
        self._log_msg(f"\n{'═'*46}\n开始转换\n命令: {' '.join(cmd)}\n{'═'*46}\n","hd")
        self._set_stat("⏳ 转换中…"); self._prog.configure(mode="indeterminate"); self._prog.start(12)
        self._running=True
        threading.Thread(target=self._run,args=(cmd,),daemon=True).start()

    def _run(self,cmd):
        env=os.environ.copy()
        exe_dir=Path(EXE).parent

        # ── PATH：exe目录（含所有DLL）──────────────────────────
        env["PATH"] = str(exe_dir) + os.pathsep + env.get("PATH","")

        # ── OSG 插件：exe_dir/osgPlugins-x.x.x ──────────────
        for d in exe_dir.iterdir() if exe_dir.exists() else []:
            if d.is_dir() and d.name.startswith("osgPlugins"):
                env["OSG_PLUGIN_PATH"] = str(d)
                break

        # ── PROJ 数据库 ──────────────────────────────────────
        proj_d = exe_dir / "proj"
        if proj_d.exists():
            env["PROJ_DATA"] = str(proj_d)
            env["PROJ_LIB"]  = str(proj_d)

        # ── GDAL 数据 ────────────────────────────────────────
        gdal_d = exe_dir / "gdal-data"
        if gdal_d.exists():
            env["GDAL_DATA"] = str(gdal_d)

        try:
            self._proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding="utf-8",errors="replace",env=env)
            total=0; done=0
            for line in self._proc.stdout:
                line=line.rstrip(); tag="dim"
                # 过滤 [DEBUG] 行：不显示在 GUI（避免卡死），但关键信息保留
                if "[DEBUG]" in line:
                    # 仅显示关键 DEBUG：纹理警告相关的上下文
                    continue
                if "OK" in line and "fail" in line: tag="ok"
                elif "ERROR" in line: tag="err"
                elif "[INFO]" in line or "[INFO " in line: tag="info"
                elif "[WARN]" in line or "[WARN " in line: tag="warn"
                m=re.search(r'\[Block (\d+)/(\d+)\]',line)
                if m: done=int(m.group(1)); total=int(m.group(2)); self._q.put(("prog",done*100//total,done,total))
                self._q.put(("log",line+"\n",tag))
            self._proc.wait(); rc=self._proc.returncode
            self._q.put(("log",f"\n{'═'*46}\n转换{'完成' if rc==0 else '失败'}，返回码: {rc}\n{'═'*46}\n","ok" if rc==0 else "err"))
            self._q.put(("done",rc))
        except Exception as e:
            self._q.put(("log",f"[ERR] 启动失败: {e}\n","err")); self._q.put(("done",-1))

    def _stop(self):
        if self._proc and self._running: self._proc.terminate(); self._log_msg("[STOP] 用户已停止\n","warn")



    def _log_msg(self,text,tag="dim"):
        self._log.configure(state="normal"); self._log.insert("end",text,tag); self._log.see("end"); self._log.configure(state="disabled")

    def _set_stat(self,text): self._stat.configure(text=text)

    def _log2_msg(self,text):
        self._log2.configure(state="normal"); self._log2.insert("end",text); self._log2.see("end"); self._log2.configure(state="disabled")

    def _log3_msg(self,text):
        self._log3.configure(state="normal"); self._log3.insert("end",text); self._log3.see("end"); self._log3.configure(state="disabled")

    def _preview_osgb(self):
        in_dir=self.v_in.get()
        if not in_dir or not os.path.isdir(in_dir):
            messagebox.showwarning("提示","请先选择 OSGB 输入目录"); return

        # 优先使用 osgb_viewer.exe（OSG 原生，直接加载 OSGB，无需转换）
        viewer_exe = None
        for candidate in [
            _HERE / "build" / "bin" / "osgb_viewer.exe",
            _HERE / "osgb_viewer.exe",
        ]:
            if candidate.exists():
                viewer_exe = str(candidate)
                break

        if viewer_exe:
            self._log2_msg(f"[INFO] 使用 OSG 原生浏览器（支持 PagedLOD 流式加载）\n{viewer_exe}\n")
            self._log2_msg(f"[INFO] 加载: {in_dir}\n")
            self._log2_msg("[TIP]  F=飞到全局 | H=切换HUD | W=线框 | L=灯光 | ESC=退出\n")
            try:
                exe_dir = Path(viewer_exe).parent
                env = os.environ.copy()
                env["PATH"] = str(exe_dir) + os.pathsep + env.get("PATH","")
                # OSG 插件目录
                for d in exe_dir.iterdir() if exe_dir.exists() else []:
                    if d.is_dir() and d.name.startswith("osgPlugins"):
                        env["OSG_PLUGIN_PATH"] = str(d); break
                subprocess.Popen([viewer_exe, in_dir], env=env)
                self._log2_msg("[OK] OSG 浏览器窗口已启动\n")
            except Exception as e:
                self._log2_msg(f"[ERR] 启动失败: {e}\n")
            return

        # ── 回退方案：转换根节点为 GLB 后用 Three.js 预览 ──
        if not os.path.isfile(EXE):
            messagebox.showwarning("提示", f"找不到 osgb2tiles.exe 且未找到 osgb_viewer.exe"); return
        self._log2_msg("[INFO] 未找到 osgb_viewer.exe，回退到 GLB 转换预览（约10-30秒）...\n")
        threading.Thread(target=self._do_osgb_preview, args=(in_dir,), daemon=True).start()



    def _do_osgb_preview(self,in_dir):
        try:
            tmp=tempfile.mkdtemp(prefix="osgb_prev_")
            cfg_p=os.path.join(tmp,"prev.json")
            prev_cfg={"inputPath":in_dir,"outputPath":tmp,"longitude":float(self.v_lon.get() or 0),
                      "latitude":float(self.v_lat.get() or 0),"height":float(self.v_alt.get() or 0),
                      "tileFormat":"glb","threads":1,"textureFormat":"jpg","maxTextureSize":1024,"jpegQuality":70}
            with open(cfg_p,"w") as f: json.dump(prev_cfg,f)
            env=os.environ.copy()
            exe_dir=Path(EXE).parent
            env["PATH"]=str(exe_dir)+os.pathsep+env.get("PATH","")
            proj_d=str(exe_dir/"proj")
            if os.path.isdir(proj_d): env["PROJ_DATA"]=proj_d; env["PROJ_LIB"]=proj_d
            cmd=[EXE,"-i",in_dir,"-o",tmp,"--config",cfg_p,
                 "--lon",str(prev_cfg["longitude"]),"--lat",str(prev_cfg["latitude"]),
                 "--format","glb","--threads","1"]
            r=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=120)
            glbs=list(Path(tmp).rglob("*.glb"))
            if not glbs:
                self._q.put(("log2","[ERR] 未生成 GLB 文件\n"))
                return
            port=start_srv(tmp)
            html_p=os.path.join(tmp,"v.html")
            with open(html_p,"w",encoding="utf-8") as f: f.write(OSGB_HTML)
            glb_rel=str(glbs[0].relative_to(tmp)).replace("\\","/")
            url=f"http://127.0.0.1:{port}/v.html?u={glb_rel}"
            self._q.put(("log2",f"[OK] 预览就绪，正在浏览器中打开...\n{url}\n"))
            self._q.put(("osgb_url",url))
        except Exception as e:
            self._q.put(("log2",f"[ERR] {e}\n"))

    def _preview_tiles(self):
        out = self.v_out.get()
        if not out or not os.path.isdir(out):
            messagebox.showwarning("提示", "请先选择输出目录"); return
        ts = os.path.join(out, "tileset.json")
        if not os.path.exists(ts):
            messagebox.showwarning("提示", "未找到 tileset.json，请先完成转换"); return
        # 若已有缓存 URL（目录未变），直接打开；否则重新挂载后再打开
        if self._tiles_preview_url:
            import time as _time
            ts_stamp = int(_time.time())
            # 只刷新时间戳，防止浏览器缓存
            url = re.sub(r't=\d+', f't={ts_stamp}', self._tiles_preview_url)
            self._log_msg(f"[OK] 3DTiles 预览: {url}\n", "ok")
            self._open_in_browser(url, self._log)
        else:
            # 尚未挂载（如手动粘贴路径），先挂载再打开
            self._mount_tiles_dir(out)
            self._log_msg("[INFO] 正在挂载数据服务，请稍候再点预览...\n", "info")



    def _open_in_browser(self,url,frame_log):
        try:
            import webview
            webview.create_window("预览",url)
            threading.Thread(target=webview.start,daemon=True).start()
        except ImportError:
            webbrowser.open(url)
            self._q.put(("log",f"[提示] 已在系统浏览器打开预览（安装 pywebview 可内嵌显示）\n","info"))

    def _poll(self):
        """每 50ms 轮询子进程队列，限速处理日志，避免 ScrolledText 批量写入阻塞主线程。"""
        # log/log2/log3 每轮最多渲染 30 条，超出直接丢弃（日志量大时不影响进度更新）
        # prog/done/url 消息不限速，始终处理
        log_count = 0
        MAX_LOG_PER_POLL = 30
        try:
            while True:
                msg = self._q.get_nowait()
                kind = msg[0]
                if kind in ("log", "log2", "log3"):
                    if log_count < MAX_LOG_PER_POLL:
                        if kind == "log":    self._log_msg(msg[1], msg[2])
                        elif kind == "log2": self._log2_msg(msg[1])
                        else:               self._log3_msg(msg[1])
                        log_count += 1
                    # 超限条目直接丢弃，避免积压导致主线程卡死
                elif kind == "prog":
                    _, pct, done, total = msg
                    self._prog.stop()
                    self._prog.configure(mode="determinate", value=pct)
                    self._set_stat(f"⏳ 转换中... {done}/{total} 块 ({pct}%)")
                elif kind == "done":
                    self._running = False
                    self._prog.stop()
                    ok = msg[1] == 0
                    self._prog.configure(mode="determinate", value=100 if ok else 0)
                    self._set_stat("✅ 转换完成！" if ok else "❌ 转换失败")
                    if ok:
                        # after() 延迟弹出，避免在 _poll 事件循环内同步阻塞
                        def _on_done():
                            out = self.v_out.get()
                            # 重新挂载（新生成的 tileset.json 已存在），然后自动预览
                            self._tiles_preview_url = None
                            self._mount_tiles_dir(out)
                            messagebox.showinfo("完成", f"3D Tiles 已生成至:\n{out}")
                            # 延迟0.5s后预览（等挂载完成）
                            self.after(500, self._preview_tiles)
                        self.after(50, _on_done)
                elif kind == "osgb_url":
                    self._open_in_browser(msg[1], self._log2)
                elif kind == "tiles_url":
                    self._open_in_browser(msg[1], self._log3)
                elif kind == "tiles_mounted":
                    _, url, has_ts = msg
                    self._tiles_preview_url = url
                    self._btn_tiles_prev.configure(state="normal" if has_ts else "disabled")
                    self._log_msg(f"[INFO] 数据服务已就绪: {url}\n", "info")
        except queue.Empty:
            pass
        self.after(50, self._poll)

if __name__=="__main__":
    try:
        from ctypes import windll; windll.shcore.SetProcessDpiAwareness(1)
    except: pass
    App().mainloop()
