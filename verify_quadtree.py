#!/usr/bin/env python3
"""
对已有的3DTiles输出目录重建全局tileset.json（验证新的四叉树合并逻辑）
"""
import json, os, re, math
from pathlib import Path

OUT_DIR = r"C:\Users\hubiao\Desktop\AIStudio\geonexus-proxy\data_examples\cbd-3dtiles-test"
GEO_ERR_SCALE = 0.5

def parse_block_coord(block_name):
    """解析 Tile_+831_+572 → (831, 572)"""
    nums = []
    i = 0
    while i < len(block_name) and len(nums) < 2:
        if block_name[i] in ('+', '-') or block_name[i].isdigit():
            if i > 0 and block_name[i-1] == '_':
                m = re.match(r'[+-]?\d+', block_name[i:])
                if m:
                    nums.append(int(m.group()))
                    i += len(m.group())
                    continue
        i += 1
    return (nums[0], nums[1]) if len(nums) >= 2 else (0, 0)

def bbox_diag(bbox):
    if bbox is None:
        return -1
    min_p, max_p = bbox
    dx = max_p[0]-min_p[0]; dy = max_p[1]-min_p[1]; dz = max_p[2]-min_p[2]
    return math.sqrt(dx*dx + dy*dy + dz*dz)

def merge_bboxes(bboxes):
    valid = [b for b in bboxes if b]
    if not valid: return None
    return (
        [min(b[0][i] for b in valid) for i in range(3)],
        [max(b[1][i] for b in valid) for i in range(3)]
    )

def bbox_to_bv(bbox, pad=0):
    if not bbox: return {"box":[0,0,200,5000,0,0,0,5000,0,0,0,500]}
    mn, mx = bbox
    cx = (mn[0]+mx[0])*0.5; cy = (mn[1]+mx[1])*0.5; cz = (mn[2]+mx[2])*0.5
    hx = (mx[0]-mn[0])*0.5+pad; hy = (mx[1]-mn[1])*0.5+pad; hz = (mx[2]-mn[2])*0.5+pad
    return {"box":[cx,cy,cz, hx,0,0, 0,hy,0, 0,0,hz]}

def auto_merge_level(n, target=16):
    if n <= target: return 0
    return min(int(math.ceil(math.log(n/target)/math.log(4))), 6)

def build_quad_tree(blocks, level, max_level):
    bboxes = [b['bbox'] for b in blocks]
    bbox = merge_bboxes(bboxes)
    diag = bbox_diag(bbox)
    
    # 叶子层
    if level >= max_level or len(blocks) == 1:
        leaf_geo_err = (diag * GEO_ERR_SCALE * 2.0) if diag > 0 else 1000.0
        node = {
            "boundingVolume": bbox_to_bv(bbox, 10),
            "geometricError": leaf_geo_err,
            "refine": "REPLACE"
        }
        if len(blocks) == 1:
            node["content"] = {"uri": blocks[0]['relpath']}
        else:
            children = []
            for b in blocks:
                child_diag = bbox_diag(b['bbox'])
                child = {
                    "boundingVolume": bbox_to_bv(b['bbox']),
                    "geometricError": (child_diag * GEO_ERR_SCALE) if child_diag > 0 else leaf_geo_err*0.5,
                    "refine": "REPLACE",
                    "content": {"uri": b['relpath']}
                }
                children.append(child)
            node["children"] = children
        return node

    # 内部节点：按坐标四分
    coords = [parse_block_coord(b['name']) for b in blocks]
    xs = [c[0] for c in coords]; ys = [c[1] for c in coords]
    xmid = (min(xs)+max(xs))//2; ymid = (min(ys)+max(ys))//2
    quads = [[] for _ in range(4)]
    for b, (bx, by) in zip(blocks, coords):
        qi = (1 if bx > xmid else 0) + (2 if by > ymid else 0)
        quads[qi].append(b)
    
    # fallback: 若全落同一象限，按索引均分
    non_empty = sum(1 for q in quads if q)
    if non_empty <= 1 and len(blocks) > 1:
        quads = [[] for _ in range(4)]
        for k, b in enumerate(blocks):
            quads[k%4].append(b)
        print(f"  [WARN] level={level}: parseCoord fallback, force-split {len(blocks)} blocks into 4 groups")

    node = {
        "boundingVolume": bbox_to_bv(bbox, 10),
        "geometricError": (diag * GEO_ERR_SCALE * 6.0) if diag > 0 else 4000.0,
        "refine": "REPLACE",
        "children": [build_quad_tree(q, level+1, max_level) for q in quads if q]
    }
    return node

# 扫描 Data/ 目录收集所有 block
data_dir = Path(OUT_DIR) / "Data"
blocks = []
for td in sorted(data_dir.iterdir()):
    ts = td / "tileset.json"
    if not ts.exists(): continue
    with open(ts, encoding='utf-8') as f:
        ts_data = json.load(f)
    bbox_box = None
    try:
        bv = ts_data['root']['boundingVolume']['box']
        cx,cy,cz,hx,_,_,_,hy,_,_,_,hz = bv
        bbox_box = ([cx-hx,cy-hy,cz-hz],[cx+hx,cy+hy,cz+hz])
    except:
        pass
    blocks.append({
        'name': td.name,
        'relpath': f"Data/{td.name}/tileset.json",
        'bbox': bbox_box
    })

print(f"扫描到 {len(blocks)} 个 Block")
ml = auto_merge_level(len(blocks))
print(f"自动合并层级: {ml}（目标<=16个叶节点）")

# 统计四叉树结构
def count_tree(node, depth=0):
    kids = node.get('children',[])
    if not kids:
        return 1
    return sum(count_tree(c, depth+1) for c in kids)

quad_root = build_quad_tree(blocks, 0, ml)
leaf_count = count_tree(quad_root)
print(f"四叉树叶节点数: {leaf_count}")

# 验证refine全为REPLACE
def check_refine(node, path="root"):
    r = node.get('refine','?')
    if r != 'REPLACE':
        print(f"  [WARN] {path} refine={r}")
    for i,c in enumerate(node.get('children',[])):
        check_refine(c, f"{path}.children[{i}]")

check_refine(quad_root)

# 验证geometricError单调递减
def check_geo_err(node, parent_err=float('inf'), path="root"):
    ge = node.get('geometricError', 0)
    if ge >= parent_err:
        print(f"  [WARN] {path} geometricError={ge:.1f} >= parent {parent_err:.1f}")
    for i,c in enumerate(node.get('children',[])):
        check_geo_err(c, ge, f"{path}.c[{i}]")

check_geo_err(quad_root)

# 打印树结构
def print_tree(node, indent=0, max_depth=4):
    ge = node.get('geometricError',0)
    ref = node.get('refine','?')
    cont = node.get('content',{}).get('uri','')
    kids = node.get('children',[])
    label = f"[{cont[:30]}]" if cont else f"[{len(kids)} children]"
    print(" "*indent + f"GeoErr={ge:.0f} ref={ref} {label}")
    if indent//2 < max_depth:
        for c in kids:
            print_tree(c, indent+2, max_depth)

print("\n=== 四叉树结构（前4层）===")
print_tree(quad_root)
print("\n✅ 验证完成")
