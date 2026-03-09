#!/usr/bin/env python3
"""
fix_b3dm_texcoord.py
--------------------
批量修复已转换的 .b3dm 文件中的 Cesium shader 错误：
  ERROR: 'v_texCoord_0' : undeclared identifier

原因：某些 glTF primitive 的 material 引用了 baseColorTexture，
但 primitive 本身没有 TEXCOORD_0 属性，导致 Cesium GLSL 编译失败。

修复：若 material 有 baseColorTexture 但对应 primitive 无 TEXCOORD_0，
则从 material 中删除 baseColorTexture 引用（geometry 仍保留，以纯白色渲染）。

用法：
    python fix_b3dm_texcoord.py <3dtiles目录>

示例：
    python fix_b3dm_texcoord.py C:/path/to/cbd-3dtiles-test

会递归扫描目录下所有 .b3dm 文件，原地修复（覆盖原文件）。
"""

import os
import sys
import json
import struct
import copy

# ── B3DM 解析 ──────────────────────────────────────────────────────────────────

B3DM_HEADER_SIZE = 28   # magic(4)+version(4)+byteLength(4)+ftJSON(4)+ftBin(4)+btJSON(4)+btBin(4)

def parse_b3dm(data: bytes):
    """解析 b3dm，返回 (header_dict, glb_bytes)"""
    if data[:4] != b'b3dm':
        raise ValueError("Not a b3dm file")
    version          = struct.unpack_from('<I', data, 4)[0]
    byte_length      = struct.unpack_from('<I', data, 8)[0]
    ft_json_len      = struct.unpack_from('<I', data, 12)[0]
    ft_bin_len       = struct.unpack_from('<I', data, 16)[0]
    bt_json_len      = struct.unpack_from('<I', data, 20)[0]
    bt_bin_len       = struct.unpack_from('<I', data, 24)[0]

    glb_offset = B3DM_HEADER_SIZE + ft_json_len + ft_bin_len + bt_json_len + bt_bin_len
    glb_bytes  = data[glb_offset:]

    header = {
        'version':    version,
        'ft_json':    data[B3DM_HEADER_SIZE : B3DM_HEADER_SIZE + ft_json_len],
        'ft_bin':     data[B3DM_HEADER_SIZE + ft_json_len : B3DM_HEADER_SIZE + ft_json_len + ft_bin_len],
        'bt_json':    data[B3DM_HEADER_SIZE + ft_json_len + ft_bin_len : B3DM_HEADER_SIZE + ft_json_len + ft_bin_len + bt_json_len],
        'bt_bin':     data[B3DM_HEADER_SIZE + ft_json_len + ft_bin_len + bt_json_len : glb_offset],
    }
    return header, glb_bytes

def build_b3dm(header: dict, glb_bytes: bytes) -> bytes:
    """重新组装 b3dm"""
    ft_json = header['ft_json']
    ft_bin  = header['ft_bin']
    bt_json = header['bt_json']
    bt_bin  = header['bt_bin']
    total   = B3DM_HEADER_SIZE + len(ft_json) + len(ft_bin) + len(bt_json) + len(bt_bin) + len(glb_bytes)
    buf = bytearray()
    buf += b'b3dm'
    buf += struct.pack('<I', header['version'])
    buf += struct.pack('<I', total)
    buf += struct.pack('<I', len(ft_json))
    buf += struct.pack('<I', len(ft_bin))
    buf += struct.pack('<I', len(bt_json))
    buf += struct.pack('<I', len(bt_bin))
    buf += ft_json + ft_bin + bt_json + bt_bin + glb_bytes
    return bytes(buf)

# ── GLB 解析 ───────────────────────────────────────────────────────────────────

def parse_glb(data: bytes):
    """解析 GLB，返回 (gltf_json_dict, bin_chunk_bytes or None)"""
    if data[:4] != b'glTF':
        raise ValueError("Not a GLB file")
    # version = struct.unpack_from('<I', data, 4)[0]
    # length  = struct.unpack_from('<I', data, 8)[0]
    offset = 12
    json_chunk_len  = struct.unpack_from('<I', data, offset)[0]
    json_chunk_type = struct.unpack_from('<I', data, offset + 4)[0]
    assert json_chunk_type == 0x4E4F534A, "Expected JSON chunk"
    json_bytes = data[offset + 8 : offset + 8 + json_chunk_len]
    gltf = json.loads(json_bytes.decode('utf-8'))

    bin_bytes = None
    offset2 = offset + 8 + json_chunk_len
    if offset2 < len(data):
        bin_chunk_len  = struct.unpack_from('<I', data, offset2)[0]
        bin_chunk_type = struct.unpack_from('<I', data, offset2 + 4)[0]
        if bin_chunk_type == 0x004E4942:  # BIN
            bin_bytes = data[offset2 + 8 : offset2 + 8 + bin_chunk_len]

    return gltf, json_bytes, bin_bytes

def build_glb(gltf: dict, bin_bytes) -> bytes:
    """重新组装 GLB"""
    json_bytes = json.dumps(gltf, separators=(',', ':')).encode('utf-8')
    # JSON 块 4 字节对齐（用空格填充）
    pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b' ' * pad

    buf = bytearray()
    # GLB 头
    total = 12 + 8 + len(json_bytes)
    if bin_bytes:
        bin_pad = (4 - len(bin_bytes) % 4) % 4
        padded_bin = bin_bytes + b'\x00' * bin_pad
        total += 8 + len(padded_bin)
    else:
        padded_bin = None

    buf += b'glTF'
    buf += struct.pack('<I', 2)      # version
    buf += struct.pack('<I', total)
    # JSON chunk
    buf += struct.pack('<I', len(json_bytes))
    buf += struct.pack('<I', 0x4E4F534A)
    buf += json_bytes
    # BIN chunk
    if padded_bin:
        buf += struct.pack('<I', len(padded_bin))
        buf += struct.pack('<I', 0x004E4942)
        buf += padded_bin

    return bytes(buf)

# ── 核心修复逻辑 ───────────────────────────────────────────────────────────────

def fix_gltf(gltf: dict) -> tuple:
    """
    检测并修复 gltf 中 material 有 baseColorTexture 但 primitive 无 TEXCOORD_0 的问题。
    返回 (fixed_gltf, fix_count)
    """
    meshes    = gltf.get('meshes', [])
    materials = gltf.get('materials', [])
    fix_count = 0

    # 记录哪些 material 索引需要去掉 baseColorTexture
    mat_needs_fix = set()

    for mesh in meshes:
        for prim in mesh.get('primitives', []):
            attrs     = prim.get('attributes', {})
            has_uv    = 'TEXCOORD_0' in attrs
            mat_idx   = prim.get('material', -1)
            if mat_idx < 0 or mat_idx >= len(materials):
                continue
            mat = materials[mat_idx]
            pbr = mat.get('pbrMetallicRoughness', {})
            has_tex = 'baseColorTexture' in pbr
            if has_tex and not has_uv:
                mat_needs_fix.add(mat_idx)

    if not mat_needs_fix:
        return gltf, 0

    # 深拷贝后修复（避免修改原对象）
    gltf = copy.deepcopy(gltf)
    materials = gltf.get('materials', [])
    for idx in mat_needs_fix:
        mat = materials[idx]
        pbr = mat.get('pbrMetallicRoughness', {})
        if 'baseColorTexture' in pbr:
            del pbr['baseColorTexture']
            fix_count += 1

    return gltf, fix_count

# ── 主流程 ──────────────────────────────────────────────────────────────────────

def process_b3dm(path: str) -> bool:
    """处理单个 b3dm 文件，返回 True 表示有修改"""
    with open(path, 'rb') as f:
        data = f.read()

    try:
        header, glb_bytes = parse_b3dm(data)
        gltf, json_bytes, bin_bytes = parse_glb(glb_bytes)
    except Exception as e:
        print(f"  [SKIP] Cannot parse: {path} — {e}")
        return False

    fixed_gltf, fix_count = fix_gltf(gltf)
    if fix_count == 0:
        return False

    new_glb   = build_glb(fixed_gltf, bin_bytes)
    new_b3dm  = build_b3dm(header, new_glb)

    with open(path, 'wb') as f:
        f.write(new_b3dm)

    print(f"  [FIXED] {os.path.basename(path)}  ({fix_count} material(s) patched)")
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        print("\n用法: python fix_b3dm_texcoord.py <3dtiles目录>")
        sys.exit(1)

    root_dir = sys.argv[1]
    if not os.path.isdir(root_dir):
        print(f"错误：目录不存在: {root_dir}")
        sys.exit(1)

    total, fixed = 0, 0
    for dirpath, _, filenames in os.walk(root_dir):
        for fname in filenames:
            if not fname.lower().endswith('.b3dm'):
                continue
            total += 1
            fpath = os.path.join(dirpath, fname)
            if process_b3dm(fpath):
                fixed += 1

    print(f"\n完成：共扫描 {total} 个 .b3dm，修复 {fixed} 个。")


if __name__ == '__main__':
    main()
