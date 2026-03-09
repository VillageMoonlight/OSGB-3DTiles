#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""修复 osgb2tiles_gui.py 中损坏的 TILES_HTML 区域"""
import re, shutil, pathlib

src = pathlib.Path('osgb2tiles_gui.py')
shutil.copy(src, src.with_suffix('.py.bak'))

text = src.read_text(encoding='utf-8')

NEW_TILES_HTML = r'''TILES_HTML=r"""<!DOCTYPE html>
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
.cesium-viewer-bottom,.cesium-credit-logoContainer,
.cesium-credit-textContainer{display:none!important}
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
  imageryProvider:false,
  terrainProvider:new Cesium.EllipsoidTerrainProvider(),
  creditContainer:document.createElement('div')
});
v.scene.globe.enableLighting=false;
v.scene.globe.baseColor=new Cesium.Color(0.06,0.06,0.1,1.0);
v.scene.globe.depthTestAgainstTerrain=false;
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
'''

# 用正则找到整个 TILES_HTML=r"""...""" 区域（跨行匹配）
pattern = re.compile(r'TILES_HTML=r""".*?"""', re.DOTALL)
matches = list(pattern.finditer(text))
print(f'Found {len(matches)} TILES_HTML block(s)')
for m in matches:
    print(f'  span: {m.start()}~{m.end()}  ({m.end()-m.start()} chars)')

if len(matches) == 0:
    print('ERROR: no TILES_HTML found!')
elif len(matches) == 1:
    new_text = text[:matches[0].start()] + NEW_TILES_HTML.strip() + '\n' + text[matches[0].end():]
    src.write_text(new_text, encoding='utf-8')
    print('Replaced 1 block OK')
else:
    # 多个匹配（损坏情况）：用第一个开始到最后一个结束来替换整段
    start = matches[0].start()
    end = matches[-1].end()
    new_text = text[:start] + NEW_TILES_HTML.strip() + '\n' + text[end:]
    src.write_text(new_text, encoding='utf-8')
    print(f'Replaced {len(matches)} blocks (merged) OK')

# 验证语法
import py_compile, sys
try:
    py_compile.compile('osgb2tiles_gui.py', doraise=True)
    print('Syntax OK')
except py_compile.PyCompileError as e:
    print(f'Syntax ERROR: {e}')
