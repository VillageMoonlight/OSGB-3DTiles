#!/usr/bin/env python3
# fix_zoomed.py  -- 启动时直接最大化，一劳永逸解决 DPI 缩放问题
with open('osgb2tiles_gui.py', 'r', encoding='utf-8') as f:
    txt = f.read()

lines = txt.splitlines(keepends=True)
out = []
for i, line in enumerate(lines):
    out.append(line)
    # 在 resizable 那行之后插入 state('zoomed')
    if 'self.resizable(True,True)' in line and 'zoomed' not in txt:
        out.append("        self.state('zoomed')  # 最大化，兼容 DPI 缩放\n")

txt2 = ''.join(out)

# 把 after(50,… 改为 after(300,… 给窗口更多时间完成布局
txt2 = txt2.replace('self.after(50, self._set_sash)', 'self.after(300, self._set_sash)')

with open('osgb2tiles_gui.py', 'w', encoding='utf-8') as f:
    f.write(txt2)

import ast
ast.parse(txt2)
print('Syntax OK, Done.')
