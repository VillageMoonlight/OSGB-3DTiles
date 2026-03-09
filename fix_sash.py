with open('osgb2tiles_gui.py', 'r', encoding='utf-8') as f:
    txt = f.read()

# 62% → 50%
txt = txt.replace(
    'self._body.sashpos(0, int(self.winfo_width() * 0.62))',
    'self._body.sashpos(0, self.winfo_width() // 2)'
)
# PanedWindow 初始宽度也改为对半
txt = txt.replace('body.add(lf,minsize=420,width=868)', 'body.add(lf,minsize=420,width=700)')
txt = txt.replace('body.add(rf,minsize=260,width=532)', 'body.add(rf,minsize=260,width=700)')

with open('osgb2tiles_gui.py', 'w', encoding='utf-8') as f:
    f.write(txt)

import ast; ast.parse(txt)
print('OK, sash=50%')
