"""Fix wchar_t* pointer arithmetic crash in main loop"""
SRC = r"src\osgb_viewer.cpp"
with open(SRC, "rb") as f:
    raw = f.read()
code = raw.decode("utf-8")
has_crlf = b"\r\n" in raw
if has_crlf:
    code = code.replace("\r\n", "\n")

# Fix the auto lb line - change to std::wstring
old = '          auto lb = s_orthoNadir ? L"\\u6e32\\u67d3DOM... "\n                                 : L"\\u6e32\\u67d3\\u6b63\\u4ea4... ";'
new = '          std::wstring lb = s_orthoNadir ? L"\\u6e32\\u67d3DOM... "\n                                           : L"\\u6e32\\u67d3\\u6b63\\u4ea4... ";'

if old in code:
    code = code.replace(old, new)
    print("OK: fixed wchar_t* to std::wstring")
else:
    print("ERROR: pattern not found, trying to find it...")
    idx = code.find("auto lb = s_orthoNadir")
    if idx >= 0:
        print(f"Found at {idx}: {repr(code[idx:idx+150])}")
    else:
        print("auto lb not found")

if has_crlf:
    code = code.replace("\n", "\r\n")
with open(SRC, "w", encoding="utf-8", newline="") as f:
    f.write(code)
