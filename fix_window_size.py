#!/usr/bin/env python3
# fix_window_size.py
with open('osgb2tiles_gui.py', 'r', encoding='utf-8') as f:
    txt = f.read()

# 1. 窗口尺寸 1340→1400
txt = txt.replace('W,H=1340,860;', 'W,H=1400,860;')
txt = txt.replace('self.minsize(1100,720)', 'self.minsize(900,640)')

# 2. 修正PanedWindow固定宽度
txt = txt.replace('body.add(lf,minsize=480,width=820)', 'body.add(lf,minsize=420)')
txt = txt.replace('body.add(rf,minsize=280,width=500)', 'body.add(rf,minsize=260)')

# 3. 添加 self._body 引用（body.pack 后）
OLD = 'body.pack(fill="both",expand=True)\n'
if 'self._body=body' not in txt:
    txt = txt.replace(OLD, OLD + '        self._body=body\n')

# 4. 添加 after(150, ...) 调用（若未存在）
if '_set_sash' not in txt:
    txt = txt.replace(
        'self._init_vars(); self._build_ui(); self._load_config(); self._poll()',
        'self._init_vars(); self._build_ui(); self._load_config(); self._poll()\n        self.after(150, self._set_sash)'
    )
    # 在 _prow 前插入 _set_sash 方法
    SASH = ('    def _set_sash(self):\n'
            '        try: self._body.sashpos(0, int(self.winfo_width() * 0.62))\n'
            '        except: pass\n\n')
    txt = txt.replace('    def _prow(self,p,label,var,cmd):', SASH + '    def _prow(self,p,label,var,cmd):')

with open('osgb2tiles_gui.py', 'w', encoding='utf-8') as f:
    f.write(txt)

# 验证关键行
for i, line in enumerate(txt.splitlines(), 1):
    if any(k in line for k in ('W,H=','minsize','_set_sash','sashpos','_body=body')):
        print(f'{i}: {line.strip()}')
print('Done.')
