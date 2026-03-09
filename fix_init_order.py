#!/usr/bin/env python3
# fix_init_order.py - 修复 __init__ 中 geometry 调用顺序和 PanedWindow 分栏宽度
with open('osgb2tiles_gui.py', 'r', encoding='utf-8') as f:
    txt = f.read()

# ── 1. __init__：把 geometry/minsize 移到 UI 构建之后 ────────────────
OLD_INIT = (
    '        self._q=queue.Queue(); self._running=False; self._proc=None\n'
    '        self._init_vars(); self._build_ui(); self._load_config(); self._poll()\n'
    '        self.after(150, self._set_sash)'
)
NEW_INIT = (
    '        self._q=queue.Queue(); self._running=False; self._proc=None\n'
    '        self._init_vars(); self._build_ui(); self._load_config(); self._poll()\n'
    '        # 构建完成后强制设置窗口尺寸（避免被内容 pack 覆盖）\n'
    '        W,H=1400,860; sw,sh=self.winfo_screenwidth(),self.winfo_screenheight()\n'
    '        self.geometry(f"{W}x{H}+{(sw-W)//2}+{(sh-H)//2}")\n'
    '        self.minsize(900,640)\n'
    '        self.update_idletasks()\n'
    '        self.after(50, self._set_sash)'
)
txt = txt.replace(OLD_INIT, NEW_INIT)

# ── 2. 删除 __init__ 开头重复的 W,H / geometry / minsize ────────────
OLD_EARLY = (
    '        W,H=1400,860; sw,sh=self.winfo_screenwidth(),self.winfo_screenheight()\n'
    '        self.geometry(f"{W}x{H}+{(sw-W)//2}+{(sh-H)//2}"); self.minsize(900,640)\n'
)
# 只删除位于 _q 之前的那一处（共可能出现两次）
idx = txt.find(OLD_EARLY)
if idx != -1:
    txt = txt[:idx] + txt[idx+len(OLD_EARLY):]

# ── 3. PanedWindow 给左右各设初始宽度（62% / 38% of 1400=868/532）────
txt = txt.replace(
    'body.add(lf,minsize=420)',
    'body.add(lf,minsize=420,width=868)'
)
txt = txt.replace(
    'body.add(rf,minsize=260)',
    'body.add(rf,minsize=260,width=532)'
)

with open('osgb2tiles_gui.py', 'w', encoding='utf-8') as f:
    f.write(txt)

# 验证关键行
for i, line in enumerate(txt.splitlines(), 1):
    if any(k in line for k in ('W,H=','geometry(','minsize','sashpos','width=868','width=532','update_idle')):
        print(f'{i}: {line.strip()}')
print('Done.')
