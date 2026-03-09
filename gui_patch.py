#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成重构后的 osgb2tiles_gui.py"""
import re, shutil, os
SRC = r'c:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles\osgb2tiles_gui.py'
BAK = SRC + '.bak'
shutil.copy2(SRC, BAK)

with open(SRC, 'r', encoding='utf-8') as f:
    txt = f.read()

# ── 1. 删除 mipmaps 变量（init_vars） ──────────────────────
txt = re.sub(r'\s*self\.v_ktx2_mipmaps=.*?\n', '\n', txt)

# ── 2. 修正 _upd_tex：删除 mipmaps 行 ──────────────────────
OLD_MIPROW = '''            # 行三：Mipmaps
            c=tk.Frame(self._rtex,bg=BG2); c.pack(fill="x",pady=(0,2))
            C(c,"生成 Mipmaps（强烈建议，减少远处闪烁走样）",self.v_ktx2_mipmaps).pack(side="left")'''
txt = txt.replace(OLD_MIPROW, '')

# ── 3. _start：删除 --config、修正7P坐标、删除 --ktx2-mipmaps ──
OLD_CMD = '''        cfg=self._build_cfg(); cfgp=self.v_cfg.get()
        cmd=[EXE,"-i",cfg["inputPath"],"-o",cfg["outputPath"],
             "--config",cfgp,
             "--lon",str(cfg["longitude"]),"--lat",str(cfg["latitude"]),"--alt",str(cfg["height"]),'''
NEW_CMD = '''        cfg=self._build_cfg()
        lon,lat,alt = self._eff_coords()
        cmd=[EXE,"-i",cfg["inputPath"],"-o",cfg["outputPath"],
             "--lon",str(lon),"--lat",str(lat),"--alt",str(alt),'''
txt = txt.replace(OLD_CMD, NEW_CMD)

OLD_KTX_MIPARG = '''        if cfg.get("textureFormat") == "ktx2":
            cmd.extend(["--ktx2-mode", cfg.get("ktx2Mode", "etc1s"),
                        "--ktx2-quality", str(cfg.get("ktx2Quality", 2)),
                        "--ktx2-mipmaps", "true" if cfg.get("ktx2Mipmaps", True) else "false"])'''
NEW_KTX_ARG = '''        if cfg.get("textureFormat") == "ktx2":
            cmd.extend(["--ktx2-mode", cfg.get("ktx2Mode","etc1s"),
                        "--ktx2-quality", str(cfg.get("ktx2Quality",2))])'''
txt = txt.replace(OLD_KTX_MIPARG, NEW_KTX_ARG)

# 删除 _build_cfg 里 ktx2Mipmaps 行
txt = re.sub(r'\s*"ktx2Mipmaps":.*?,\n', '\n', txt)

# ── 4. _save_cfg 前插入 _eff_coords 和 _calc7p 方法 ──────────
NEW_METHODS = '''
    def _eff_coords(self):
        """返回实际传给主程序的 (lon, lat, alt)：若七参数启用则返回转换后坐标"""
        lon=float(self.v_lon.get() or 0)
        lat=float(self.v_lat.get() or 0)
        alt=float(self.v_alt.get() or 0)
        if not self.v_7p.get(): return lon, lat, alt
        r = self._calc7p(lon, lat, alt)
        return r if r[0] is not None else (lon, lat, alt)

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

'''
txt = txt.replace("    def _save_cfg(self):", NEW_METHODS + "    def _save_cfg(self):")

# ── 5. _tog7p：触发坐标刷新 ──────────────────────────────────
OLD_TOG = '''    def _tog7p(self):
        st="normal" if self.v_7p.get() else "disabled"
        for child in self._f7.winfo_children():
            for w in child.winfo_children():
                try: w.configure(state=st)
                except: pass'''
NEW_TOG = '''    def _tog7p(self, *_):
        st="normal" if self.v_7p.get() else "disabled"
        for child in self._f7.winfo_children():
            for w in child.winfo_children():
                try: w.configure(state=st)
                except: pass
        self._upd_7p_result()

    def _upd_7p_result(self, *_):
        if not self.v_7p.get():
            self.v_7p_result.set("")
            return
        try:
            lon=float(self.v_lon.get() or 0)
            lat=float(self.v_lat.get() or 0)
            alt=float(self.v_alt.get() or 0)
        except: return
        r = self._calc7p(lon, lat, alt)
        if r[0] is not None:
            self.v_7p_result.set(f"WGS84: lon={r[0]:.6f}°  lat={r[1]:.6f}°  alt={r[2]:.1f}m")
        else:
            self.v_7p_result.set("（需安装 pyproj：pip install pyproj）")'''
txt = txt.replace(OLD_TOG, NEW_TOG)

# ── 6. _init_vars：添加 v_7p_result ──────────────────────────
txt = txt.replace(
    "        self.v_sc=tk.StringVar(value=\"0.0\")",
    "        self.v_sc=tk.StringVar(value=\"0.0\")\n        self.v_7p_result=tk.StringVar(value=\"\")"
)

# ── 7. 七参数面板：添加结果显示行 ──────────────────────────────
OLD_7P_END = "        self._tog7p()"
NEW_7P_END = '''        # 转换结果显示（只读）
        rr=tk.Frame(tf,bg=BG2); rr.pack(fill="x",pady=(4,0))
        tk.Label(rr,textvariable=self.v_7p_result,fg=GRN,bg=BG2,font=("Microsoft YaHei UI",9)).pack(side="left")
        # 参数变化时自动刷新
        for v in [self.v_dx,self.v_dy,self.v_dz,self.v_rx,self.v_ry,self.v_rz,self.v_sc,
                  self.v_lon,self.v_lat,self.v_alt]:
            v.trace_add("write", self._upd_7p_result)
        self._tog7p()'''
txt = txt.replace(OLD_7P_END, NEW_7P_END, 1)

# ── 8. _load_from：删除 ktx2Mipmaps 加载行 ──────────────────
txt = re.sub(r'\s*self\.v_ktx2_mipmaps\.set.*?\n', '\n', txt)

# ── 9. 重构 _build_ui：左参数区→Notebook，右→日志 ─────────────
OLD_BODY = '''        body=tk.PanedWindow(self,orient="horizontal",bg=BG,sashwidth=5,sashrelief="flat")
        body.pack(fill="both",expand=True)

        # ── 左侧参数面板 ──
        lf=tk.Frame(body,bg=BG); body.add(lf,minsize=560,width=760)
        lp=tk.Frame(lf,bg=BG,padx=12,pady=8); lp.pack(fill="both",expand=True)

        # § 路径
        SEC(lp,"路径配置","📁").pack(fill="x",pady=(0,2))
        pf=tk.Frame(lp,bg=BG2,padx=10,pady=8); pf.pack(fill="x")
        self._prow(pf,"OSGB 输入目录:",self.v_in,self._browse_in)
        self._prow(pf,"3DTiles 输出目录:",self.v_out,self._browse_out)'''

NEW_BODY = '''        body=tk.PanedWindow(self,orient="horizontal",bg=BG,sashwidth=6,sashrelief="flat")
        body.pack(fill="both",expand=True)

        # ── 左：双Tab参数区（主体3/5）──
        lf=tk.Frame(body,bg=BG); body.add(lf,minsize=480,width=820)
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
        self._btn_tiles_prev.configure(state="disabled")'''

txt = txt.replace(OLD_BODY, NEW_BODY)

# ── 9b. 原分节§坐标原点到七参数结束，改为在Tab1下（不是lf顶层） ─
# 路径后面的内容已经在lp里，不用额外改，lp指向Tab1的Frame即可

# ── 9c. 转换选项节移到Tab2 ────────────────────────────────
OLD_SEC_OPT = "        # § 转换选项\n        SEC(lp,\"转换选项\",\"⚙️\").pack(fill=\"x\",pady=(8,2))"
NEW_SEC_OPT = '''        # ─ Tab2：转换选项 ─
        t2=tk.Frame(self._tabs,bg=BG); self._tabs.add(t2,text="  🛠 转换选项  ")
        lp2=tk.Frame(t2,bg=BG); lp2.pack(fill="both",expand=True,padx=10,pady=6)
        lp=lp2  # 后续 of/r1..r6/配置文件 都挂到 lp2

        # § 输出格式
        SEC(lp,"输出格式 & 线程","📄").pack(fill="x",pady=(0,2))'''
txt = txt.replace(OLD_SEC_OPT, NEW_SEC_OPT)

# 配置文件节后的 Notebook 部分（右侧→改为日志Frame）
OLD_RIGHT = '''        # ── 右侧：Notebook ──
        rf=tk.Frame(body,bg=BG2); body.add(rf,minsize=380,width=560)
        self._nb=ttk.Notebook(rf); self._nb.pack(fill="both",expand=True,padx=6,pady=6)

        # Tab1：日志
        t1=tk.Frame(self._nb,bg=BG2); self._nb.add(t1,text="  📋 转换日志  ")
        lh=tk.Frame(t1,bg=BG2); lh.pack(fill="x",padx=8,pady=(6,2))
        L(lh,"转换日志",fg=SKY,sz=10).pack(side="left")
        B(lh,"清空",self._clear_log,w=5,bg=SURF).pack(side="right")
        self._log=scrolledtext.ScrolledText(t1,bg=BG2,fg=FG,font=("Consolas",9),wrap="word",state="disabled",relief="flat",padx=6,pady=4)
        self._log.pack(fill="both",expand=True,padx=4,pady=4)
        for tag,fg_c in [("ok",GRN),("err",RED),("info",BLU),("warn",YLW),("dim",FGM),("hd",SKY)]:
            self._log.tag_configure(tag,foreground=fg_c)

        # Tab2：OSGB预览
        t2=tk.Frame(self._nb,bg=BG2); self._nb.add(t2,text="  🏔 OSGB预览  ")
        ph=tk.Frame(t2,bg=BG2); ph.pack(fill="x",padx=8,pady=6)
        L(ph,"⚠ 仅加载第一个根节点作为粗模，用于快速确认位置和基本外观",fg=YLW,sz=8).pack(side="left")
        B(ph,"▶ 加载预览",self._preview_osgb,w=10,bg=SURF).pack(side="right")
        self._log2=scrolledtext.ScrolledText(t2,bg="#0d1117",fg=FGS,font=("Consolas",9),wrap="word",state="disabled",relief="flat",height=6)
        self._log2.pack(fill="x",padx=4)
        tk.Label(t2,text="预览将在系统浏览器中打开（Three.js WebGL）\\n如需内嵌预览请安装：pip install pywebview",
                 fg=FGM,bg=BG2,font=("Microsoft YaHei UI",9)).pack(pady=8)

        # Tab3：3DTiles预览
        t3=tk.Frame(self._nb,bg=BG2); self._nb.add(t3,text="  🌍 3DTiles预览  ")
        th3=tk.Frame(t3,bg=BG2); th3.pack(fill="x",padx=8,pady=6)
        L(th3,"转换完成后自动激活，或手动点击加载输出目录",fg=FGM,sz=8).pack(side="left")
        B(th3,"▶ 加载预览",self._preview_tiles,w=10,bg=SURF).pack(side="right")
        self._log3=scrolledtext.ScrolledText(t3,bg="#0d1117",fg=FGS,font=("Consolas",9),wrap="word",state="disabled",relief="flat",height=6)
        self._log3.pack(fill="x",padx=4)
        tk.Label(t3,text="预览将在系统浏览器中打开（CesiumJS WebGL）\\n如需内嵌预览请安装：pip install pywebview",
                 fg=FGM,bg=BG2,font=("Microsoft YaHei UI",9)).pack(pady=8)'''

NEW_RIGHT = '''        # ── 右：转换日志（2/5）──
        rf=tk.Frame(body,bg=BG2); body.add(rf,minsize=280,width=500)
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
        self._nb=self._tabs  # 兼容旧引用'''

txt = txt.replace(OLD_RIGHT, NEW_RIGHT)

# ── 10. 修改 _prow，添加 _prow_preview ────────────────────
OLD_PROW = '''    def _prow(self,p,label,var,cmd):
        row=tk.Frame(p,bg=BG2); row.pack(fill="x",pady=3)
        tk.Label(row,text=label,fg=BLU,bg=BG2,width=16,anchor="e",font=("Microsoft YaHei UI",9)).pack(side="left")
        E(row,var,w=48).pack(side="left",padx=(6,4))
        B(row,"📂 选择",cmd,w=8).pack(side="left")'''

NEW_PROW = '''    def _prow(self,p,label,var,cmd):
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
        setattr(self,btn_attr,btn)'''

txt = txt.replace(OLD_PROW, NEW_PROW)

# ── 11. _browse_in：更新后启用OSGB预览按钮 ──────────────────
OLD_BROWSE = '''    def _browse_in(self):
        d=filedialog.askdirectory(title="选择 OSGB 输入目录")
        if d: self.v_in.set(d); self._auto_coord(d)'''
NEW_BROWSE = '''    def _browse_in(self):
        d=filedialog.askdirectory(title="选择 OSGB 输入目录")
        if d:
            self.v_in.set(d); self._auto_coord(d)
            self._btn_osgb_prev.configure(state="normal")'''
txt = txt.replace(OLD_BROWSE, NEW_BROWSE)

# ── 12. 转换完成后启用预览按钮 ──────────────────────────────
OLD_ON_DONE = '''                        messagebox.showinfo("完成", f"3D Tiles 已生成至:\\n{self.v_out.get()}")
                            self._nb.select(2)
                            self._preview_tiles()'''
NEW_ON_DONE = '''                        self._btn_tiles_prev.configure(state="normal")
                            messagebox.showinfo("完成", f"3D Tiles 已生成至:\\n{self.v_out.get()}")
                            self._preview_tiles()'''
txt = txt.replace(OLD_ON_DONE, NEW_ON_DONE)

# ── 13. _preview_osgb：不再切换 nb tab ──────────────────────
txt = txt.replace("        self._nb.select(1)\n", "")

# ── 14. _preview_tiles：不再切换 nb tab ──────────────────────
txt = txt.replace("        self._nb.select(2)\n", "")

with open(SRC, 'w', encoding='utf-8') as f:
    f.write(txt)
print("gui_patch.py: Done!")
