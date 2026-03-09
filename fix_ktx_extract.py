"""
手动解压 KTX-Software tarball 到 vcpkg buildtrees
兼容 Python 3.12+ 的安全过滤器，完全跳过符号链接，只提取普通文件
"""
import urllib.request
import tarfile
import os
import sys
import io

TARBALL_URL    = "https://github.com/KhronosGroup/KTX-Software/archive/v4.4.2.tar.gz"
DOWNLOADS_DIR  = r"C:\vcpkg\downloads"
TARBALL_NAME   = "KhronosGroup-KTX-Software-v4.4.2.tar.gz"
TARBALL_PATH   = os.path.join(DOWNLOADS_DIR, TARBALL_NAME)
BUILDTREES_SRC = r"C:\vcpkg\buildtrees\ktx\src"
CLEAN_DIR_NAME = "v4.4.2-16f9ed2e07.clean"
CLEAN_DIR      = os.path.join(BUILDTREES_SRC, CLEAN_DIR_NAME)

def download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 100_000:
        print(f"[SKIP] Already downloaded: {dest} ({os.path.getsize(dest)//1024//1024}MB)")
        return
    print(f"[DL] Downloading {url}  ...")
    last = [0]
    def progress(count, block, total):
        mb = count * block // 1024 // 1024
        if mb != last[0]:
            last[0] = mb
            sys.stdout.write(f"\r    {mb}MB downloaded ..."); sys.stdout.flush()
    urllib.request.urlretrieve(url, dest, progress)
    print(f"\n[OK] Download complete ({os.path.getsize(dest)//1024//1024}MB)")

def extract_safe(tarball, dest):
    if os.path.isdir(dest) and len(os.listdir(dest)) > 100:
        print(f"[SKIP] Already extracted: {dest}")
        return
    os.makedirs(dest, exist_ok=True)
    print(f"[EX] Extracting to {dest} ...")
    ok = skip = err = 0
    with tarfile.open(tarball, "r:gz") as tf:
        members = tf.getmembers()
        total = len(members)
        for i, m in enumerate(members):
            # 跳过所有符号链接和硬链接（Windows 不支持）
            if m.issym() or m.islnk():
                skip += 1
                continue
            # 去掉第一级目录前缀
            parts = m.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            rel_path = parts[1]
            # 只处理普通文件和目录
            if m.isdir():
                target_dir = os.path.join(dest, rel_path.replace("/", os.sep))
                os.makedirs(target_dir, exist_ok=True)
                ok += 1
            elif m.isfile():
                target_path = os.path.join(dest, rel_path.replace("/", os.sep))
                target_dir2 = os.path.dirname(target_path)
                os.makedirs(target_dir2, exist_ok=True)
                try:
                    fobj = tf.extractfile(m)
                    if fobj:
                        data = fobj.read()
                        with open(target_path, 'wb') as out:
                            out.write(data)
                        ok += 1
                except Exception as e:
                    err += 1
                    if err < 5:
                        print(f"\n  [WARN] {rel_path}: {e}")
            if i % 200 == 0:
                sys.stdout.write(f"\r   {i}/{total}  ok={ok} skip={skip} err={err}")
                sys.stdout.flush()
    print(f"\n[OK] Extracted: ok={ok}  symlinks skipped={skip}  errors={err}")
    print(f"Files in dest: {len(os.listdir(dest))}")

if __name__ == "__main__":
    os.makedirs(DOWNLOADS_DIR, exist_ok=True)
    os.makedirs(BUILDTREES_SRC, exist_ok=True)
    download(TARBALL_URL, TARBALL_PATH)
    extract_safe(TARBALL_PATH, CLEAN_DIR)
    print(f"\n[DONE] Source ready at:\n  {CLEAN_DIR}")
    print("\n接下来在 VS 命令提示符 build/ 目录执行: cmake .. && nmake")
