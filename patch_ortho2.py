"""Fix glReadPixels -> osg::Image::readPixels"""
SRC = r"src\osgb_viewer.cpp"
with open(SRC, "r", encoding="utf-8") as f:
    code = f.read()

old = "      glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, dst->data());"
new = "      dst->readPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE);"

if old in code:
    code = code.replace(old, new)
    with open(SRC, "w", encoding="utf-8", newline="") as f:
        f.write(code)
    print("OK: replaced glReadPixels with osg::Image::readPixels")
else:
    print("ERROR: pattern not found")
    # Debug
    idx = code.find("glReadPixels")
    if idx >= 0:
        print(f"Found glReadPixels at {idx}: {repr(code[idx:idx+80])}")
    else:
        print("glReadPixels not found in file at all")
