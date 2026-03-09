"""
生成 OSGB 转换工具图标
主题：无人机（四旋翼）+ 3D 数字地形 + 渐变圆形底盘
"""
import math, sys
try:
    from PIL import Image, ImageDraw, ImageFont
    PIL_OK = True
except ImportError:
    PIL_OK = False

def draw_icon(size=256):
    img  = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    s    = size
    cx, cy = s//2, s//2

    # ── 圆形渐变底盘 ──────────────────────────────
    for r in range(s//2, 0, -1):
        t = 1.0 - r / (s/2)
        R = int(20  + (44-20)*t)
        G = int(22  + (62-22)*t)
        B = int(48  + (120-48)*t)
        draw.ellipse([cx-r, cy-r, cx+r, cy+r], fill=(R,G,B,255))

    # 外圈光晕
    for i in range(8):
        r0 = int(s*0.47); r1 = int(s*0.495)
        a  = 255 - i*28
        draw.ellipse([cx-r0-i, cy-r0-i, cx+r0+i, cy+r0+i],
                     outline=(100, 180, 255, a), width=1)

    # ── 地面网格（3D Tiles 象征）────────────────────
    grid_pts = []
    grid_color = (74, 144, 226, 160)
    # 菱形透视网格
    ox, oy  = cx, int(cy + s*0.14)
    gx, gy  = int(s*0.28), int(s*0.12)  # half-width, half-depth
    ht      = int(s*0.08)
    rows, cols = 3, 3
    for r in range(rows+1):
        for c in range(cols+1):
            x0 = ox + (c - cols/2)*gx//cols*2
            y0 = oy + (r - rows/2 + c*0.5 - 0.75)*gy
            x1 = ox + (c+1 - cols/2)*gx//cols*2
            y1 = oy + (r - rows/2 + (c+1)*0.5 - 0.75)*gy
            if c < cols:
                draw.line([int(x0),int(y0), int(x1),int(y1)], fill=grid_color, width=1)
            if r < rows and c < cols:
                x2 = ox + (c - cols/2)*gx//cols*2
                y2 = oy + (r+1 - rows/2 + c*0.5 - 0.75)*gy
                draw.line([int(x0),int(y0), int(x2),int(y2)], fill=grid_color, width=1)

    # 几个突出的 3D 柱子（象征建筑/地物）
    buildings = [
        (cx - int(s*0.12), oy - int(s*0.01), int(s*0.07), int(s*0.10), (100, 200, 255, 200)),
        (cx + int(s*0.04), oy - int(s*0.03), int(s*0.06), int(s*0.14), (80, 160, 240, 200)),
        (cx - int(s*0.04), oy + int(s*0.02), int(s*0.05), int(s*0.08), (120, 210, 255, 180)),
    ]
    for bx, by, bw, bh, bc in buildings:
        # 正面
        draw.polygon([(bx, by), (bx+bw, by), (bx+bw, by+bh), (bx, by+bh)], fill=bc)
        # 顶面（斜面）
        top_c = (min(bc[0]+40,255), min(bc[1]+40,255), min(bc[2]+40,255), bc[3])
        draw.polygon([(bx, by), (bx+bw, by),
                      (bx+bw+int(bw*0.3), by-int(bh*0.25)),
                      (bx+int(bw*0.3), by-int(bh*0.25))], fill=top_c)

    # ── 无人机主体 ──────────────────────────────────
    # 机身（小圆角矩形，居中偏上）
    bw2, bh2 = int(s*0.10), int(s*0.06)
    body_x = cx - bw2//2
    body_y = int(cy - s*0.15) - bh2//2
    body_color = (220, 235, 255, 255)
    draw.rounded_rectangle([body_x, body_y, body_x+bw2, body_y+bh2],
                            radius=6, fill=body_color)
    # 机身中心摄像头（小圆）
    cam_r = int(s*0.022)
    draw.ellipse([cx-cam_r, body_y+bh2//2-cam_r, cx+cam_r, body_y+bh2//2+cam_r],
                 fill=(30, 30, 50, 255), outline=(100,180,255,255), width=2)
    # 镜头高光
    draw.ellipse([cx-cam_r+3, body_y+bh2//2-cam_r+3,
                  cx-cam_r+8, body_y+bh2//2-cam_r+8],
                 fill=(255, 255, 255, 180))

    # 四条机臂（X形）
    arm_len  = int(s*0.17)
    arm_w    = 3
    arm_color = (170, 200, 240, 255)
    arm_cx = cx
    arm_cy = body_y + bh2//2
    angles = [45, 135, 225, 315]
    arm_ends = []
    for ang in angles:
        ex = int(arm_cx + arm_len * math.cos(math.radians(ang)))
        ey = int(arm_cy + arm_len * math.sin(math.radians(ang)))
        draw.line([arm_cx, arm_cy, ex, ey], fill=arm_color, width=arm_w)
        arm_ends.append((ex, ey))

    # 四个电机支架 + 螺旋桨
    prop_r   = int(s*0.10)
    prop_w   = int(s*0.018)
    for i, (ex, ey) in enumerate(arm_ends):
        # 电机（小圆）
        mot_r = int(s*0.025)
        draw.ellipse([ex-mot_r, ey-mot_r, ex+mot_r, ey+mot_r],
                     fill=(80, 120, 180, 255), outline=(150,200,255,200), width=2)
        # 螺旋桨（两片椭圆）
        prop_tilt = 30 if i%2==0 else -30
        for blade_ang in [prop_tilt, prop_tilt+180]:
            bax = math.radians(blade_ang)
            px1 = ex + int(prop_r * 0.1 * math.cos(bax))
            py1 = ey + int(prop_r * 0.1 * math.sin(bax))
            px2 = ex + int(prop_r * math.cos(bax))
            py2 = ey + int(prop_r * math.sin(bax))
            # 桨叶宽度方向（垂直旋转方向）
            perp = math.radians(blade_ang + 90)
            pw = prop_w
            bld = [
                (int(px1 - pw*math.cos(perp)), int(py1 - pw*math.sin(perp))),
                (int(px2 - pw*0.3*math.cos(perp)), int(py2 - pw*0.3*math.sin(perp))),
                (int(px2 + pw*0.3*math.cos(perp)), int(py2 + pw*0.3*math.sin(perp))),
                (int(px1 + pw*math.cos(perp)), int(py1 + pw*math.sin(perp))),
            ]
            alpha = 180 if i%2==0 else 160
            draw.polygon(bld, fill=(160, 220, 255, alpha))

    # ── 扫描线光效（科技感）───────────────────────────
    # 无人机到地面的激光线
    scan_color = (100, 220, 255, 80)
    laser_pts = [
        (cx, body_y + bh2),
        (cx - int(s*0.12), oy - int(s*0.03)),
        (cx + int(s*0.12), oy - int(s*0.03)),
    ]
    draw.polygon(laser_pts, fill=(100, 200, 255, 30))
    draw.line([cx, body_y+bh2, cx, oy-int(s*0.03)], fill=(100,220,255,120), width=1)

    # ── 文字 "3D" ───────────────────────────────────
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", int(s*0.12))
    except:
        font = ImageFont.load_default()
    txt = "3D"
    bbox_t = draw.textbbox((0,0), txt, font=font)
    tw, th = bbox_t[2]-bbox_t[0], bbox_t[3]-bbox_t[1]
    # 在底部绘制
    tx = cx - tw//2
    ty = oy + int(s*0.08)
    # 文字阴影
    draw.text((tx+2, ty+2), txt, font=font, fill=(0,80,160,100))
    draw.text((tx, ty), txt, font=font, fill=(130, 210, 255, 255))

    return img


def make_ico(out_path):
    sizes = [256, 128, 64, 48, 32, 16]
    imgs  = []
    base  = draw_icon(256)
    for sz in sizes:
        imgs.append(base.resize((sz, sz), Image.LANCZOS))
    imgs[0].save(out_path, format="ICO",
                 sizes=[(s, s) for s in sizes],
                 append_images=imgs[1:])
    print(f"图标已保存: {out_path}")


if __name__ == "__main__":
    if not PIL_OK:
        print("PIL 未安装，尝试安装...")
        import subprocess
        subprocess.run(["python", "-m", "pip", "install", "Pillow", "-q"])
        from PIL import Image, ImageDraw, ImageFont
    
    out = sys.argv[1] if len(sys.argv) > 1 else "osgb2tiles_gui.ico"
    make_ico(out)
    # 同时保存 PNG 预览
    draw_icon(512).save(out.replace(".ico", "_preview.png"))
    print("预览: " + out.replace(".ico", "_preview.png"))
