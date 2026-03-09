#!/usr/bin/env python3
"""
fix_tileset_bbox.py
-------------------
修复 3D Tiles 全局 tileset.json 的 root boundingVolume：
将硬编码的占位盒 (0,0,200/±2000m) 改为从所有子节点的 bounding box 合并计算。

用法：
    python fix_tileset_bbox.py <3dtiles目录>

示例：
    python fix_tileset_bbox.py C:/path/to/cbd-3dtiles-test

原地修改 tileset.json，同时递归修复每个 Block 的 tileset.json 中有类似问题的 root bbox。
"""

import sys
import os
import json
import math

def box_to_aabb(box):
    """
    将 glTF box 格式 [cx,cy,cz, hx,0,0, 0,hy,0, 0,0,hz]
    转换为 (min_x, min_y, min_z, max_x, max_y, max_z)
    """
    cx, cy, cz = box[0], box[1], box[2]
    hx = abs(box[3])
    hy = abs(box[7])
    hz = abs(box[11])
    return (cx - hx, cy - hy, cz - hz,
            cx + hx, cy + hy, cz + hz)

def union_aabb(a, b):
    return (min(a[0], b[0]), min(a[1], b[1]), min(a[2], b[2]),
            max(a[3], b[3]), max(a[4], b[4]), max(a[5], b[5]))

def aabb_to_box(aabb):
    min_x, min_y, min_z, max_x, max_y, max_z = aabb
    cx = (min_x + max_x) / 2
    cy = (min_y + max_y) / 2
    cz = (min_z + max_z) / 2
    hx = (max_x - min_x) / 2
    hy = (max_y - min_y) / 2
    hz = (max_z - min_z) / 2
    return [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]

def collect_tile_bbox(tile, base_dir, visited=None):
    """
    递归收集一个 tile（及其 children）的 AABB，
    若有 content.uri 指向另一个 tileset.json，也递归进去。
    """
    if visited is None:
        visited = set()

    aabb = None

    # 当前 tile 的 boundingVolume.box
    bv = tile.get('boundingVolume', {})
    box = bv.get('box')
    if box and len(box) >= 12:
        try:
            cur = box_to_aabb(box)
            # 过滤掉明显的占位值（中心在0,0,0 且半径<=2000）
            if not (abs(cur[0]) <= 2000 and abs(cur[1]) <= 2000 and abs(cur[3]) <= 2000 and abs(cur[4]) <= 2000
                    and max(abs(cur[3] - cur[0]), abs(cur[4] - cur[1])) <= 4000):
                aabb = cur
        except Exception:
            pass

    # 子 tile 中的 content.uri 可能是另一个 tileset.json
    content = tile.get('content', {})
    uri = content.get('uri', '')
    if uri.endswith('tileset.json'):
        child_path = os.path.normpath(os.path.join(base_dir, uri))
        if child_path not in visited and os.path.isfile(child_path):
            visited.add(child_path)
            child_ts = load_json(child_path)
            if child_ts:
                child_root = child_ts.get('root', {})
                child_aabb = collect_tile_bbox(child_root, os.path.dirname(child_path), visited)
                if child_aabb:
                    aabb = union_aabb(aabb, child_aabb) if aabb else child_aabb

    # 递归处理当前 tile 的 children
    for child in tile.get('children', []):
        child_aabb = collect_tile_bbox(child, base_dir, visited)
        if child_aabb:
            aabb = union_aabb(aabb, child_aabb) if aabb else child_aabb

    return aabb

def load_json(path):
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        print(f"  [ERR] Cannot load {path}: {e}")
        return None

def fix_root_bbox(tileset_path):
    """
    读取 tileset.json，从所有 children 合并 AABB 后更新 root boundingVolume.box。
    返回 True 表示有修改。
    """
    ts = load_json(tileset_path)
    if not ts:
        return False

    root = ts.get('root', {})
    base_dir = os.path.dirname(tileset_path)

    # 收集所有 children 的 AABB（不包含当前 root 的）
    merged_aabb = None
    for child in root.get('children', []):
        child_aabb = collect_tile_bbox(child, base_dir)
        if child_aabb:
            merged_aabb = union_aabb(merged_aabb, child_aabb) if merged_aabb else child_aabb

    if not merged_aabb:
        print(f"  [SKIP] No valid child bbox found: {tileset_path}")
        return False

    # 添加少量 padding
    pad = 100  # 100m
    padded = (merged_aabb[0]-pad, merged_aabb[1]-pad, merged_aabb[2]-pad,
              merged_aabb[3]+pad, merged_aabb[4]+pad, merged_aabb[5]+pad)

    new_box = aabb_to_box(padded)
    old_box = root.get('boundingVolume', {}).get('box', None)

    # 检查是否需要更新（只在 root bbox 不包含 merged_aabb 时才更新）
    if old_box and len(old_box) >= 12:
        old_aabb = box_to_aabb(old_box)
        if (old_aabb[0] <= merged_aabb[0] - 1 and old_aabb[1] <= merged_aabb[1] - 1 and
                old_aabb[3] >= merged_aabb[3] + 1 and old_aabb[4] >= merged_aabb[4] + 1):
            # 已正确包含，无需修改
            return False

    root.setdefault('boundingVolume', {})['box'] = new_box
    ts['root'] = root

    with open(tileset_path, 'w', encoding='utf-8') as f:
        json.dump(ts, f, separators=(',', ':'), indent=2)

    cx, cy = new_box[0], new_box[1]
    hx, hy = abs(new_box[3]), abs(new_box[7])
    print(f"  [FIXED] {os.path.basename(tileset_path)}: "
          f"root bbox center=({cx:.0f},{cy:.0f}) half=({hx:.0f},{hy:.0f})")
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    root_dir = sys.argv[1]
    if not os.path.isdir(root_dir):
        print(f"错误：目录不存在: {root_dir}")
        sys.exit(1)

    # 1. 修复全局 tileset.json
    global_ts_path = os.path.join(root_dir, 'tileset.json')
    if os.path.isfile(global_ts_path):
        print(f"修复全局 tileset.json: {global_ts_path}")
        fix_root_bbox(global_ts_path)
    else:
        print(f"未找到全局 tileset.json: {global_ts_path}")

    # 2. 递归修复所有 Block 级 tileset.json
    total, fixed = 0, 0
    for dirpath, dirs, files in os.walk(root_dir):
        if dirpath == root_dir:
            continue  # 全局已处理
        for fname in files:
            if fname == 'tileset.json':
                total += 1
                path = os.path.join(dirpath, fname)
                if fix_root_bbox(path):
                    fixed += 1

    print(f"\n完成：修复全局 tileset + {fixed}/{total} 个 Block tileset.json。")


if __name__ == '__main__':
    main()
