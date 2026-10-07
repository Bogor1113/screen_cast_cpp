# -*- coding: utf-8 -*-
"""
ScreenCast 炫酷图标生成器 (v2 霓虹发光风)
=========================================
学生端 (student.ico): 深空底 + 青蓝霓虹描边 + "显示器+播放"符号 (观看)
教师端 (teacher.ico): 深空底 + 橙粉霓虹描边 + "播放+广播波纹"符号 (推流)

元素:
  - 深空径向渐变底 (中心偏亮 -> 边缘近黑)
  - 圆角方形 + 渐变霓虹描边环 (外侧高斯模糊光晕)
  - 中央白色符号 + 同色光晕 (霓虹灯管感)
  - 顶部玻璃高光 + 内描边

输出: 16/24/32/48/64/128/256 px 多尺寸 ICO (256 以 PNG 压缩存储) + PNG 预览图。
全部图形以 4x 超采样绘制后降采样, 保证小尺寸下边缘平滑。
"""
from PIL import Image, ImageDraw, ImageChops, ImageFilter

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4                      # 超采样倍率
MASTER = 256 * SS           # 1024 主画布

# ---------------- 工具函数 ----------------


def diag_gradient(size, c1, c2, grid=64):
    """对角线渐变 (c1 左上 -> c2 右下), 低分辨率计算后放大, 平滑且快。"""
    img = Image.new("RGB", (grid, grid))
    px = img.load()
    denom = 2 * (grid - 1)
    for y in range(grid):
        for x in range(grid):
            t = (x + y) / denom
            px[x, y] = tuple(int(a + (b - a) * t) for a, b in zip(c1, c2))
    return img.resize((size, size), Image.BICUBIC)


def radial_dark(size, center=(16, 20, 44), edge=(4, 5, 10), grid=128):
    """深空径向渐变: 中心偏亮深蓝 -> 边缘近黑。"""
    img = Image.new("RGB", (grid, grid))
    px = img.load()
    cx = cy = (grid - 1) / 2.0
    maxd = cx * 1.06
    for y in range(grid):
        for x in range(grid):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / maxd
            t = min(1.0, d * d)          # 平方衰减 -> 边缘迅速变黑
            px[x, y] = tuple(int(a + (b - a) * t) for a, b in zip(center, edge))
    return img.resize((size, size), Image.BICUBIC)


def rounded_mask(size, radius_ratio=0.22):
    """圆角方形 alpha 蒙版。"""
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=int(size * radius_ratio), fill=255)
    return m


def glass_overlay(size, mask, peak=58, floor=0.6):
    """顶部玻璃高光: 从顶部按指数衰减到 0, 且只出现在圆角蒙版内。"""
    grad = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(grad)
    limit = int(size * floor)
    for y in range(limit):
        t = max(0.0, 1.0 - y / limit)
        a = int(peak * (t ** 1.6))
        if a:
            d.line([(0, y), (size, y)], fill=a)
    white = Image.new("RGBA", (size, size), (255, 255, 255, 255))
    transparent = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    return Image.composite(white, transparent, ImageChops.multiply(grad, mask))


def neon_ring(size, c1, c2, width_ratio=0.055):
    """渐变霓虹描边环 (RGBA): 圆角外框减去内框得到环形遮罩, 填入对角渐变。"""
    m_outer = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m_outer).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=int(size * 0.22), fill=255)
    inset = max(1, int(size * width_ratio))
    m_inner = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m_inner).rounded_rectangle(
        [inset, inset, size - 1 - inset, size - 1 - inset],
        radius=max(1, int((size - 2 * inset) * 0.22)), fill=255)
    ring_mask = ImageChops.subtract(m_outer, m_inner)
    ring = diag_gradient(size, c1, c2).convert("RGBA")
    ring.putalpha(ring_mask)
    return ring


def glow(shape_rgba, color, blur_ratio, strength=1.0):
    """把 alpha 形状染成 color 并高斯模糊, 得到同色光晕层。"""
    a = shape_rgba.split()[3].point(lambda v: int(v * strength))
    tint = Image.new("RGBA", shape_rgba.size, tuple(color) + (255,))
    transparent = Image.new("RGBA", shape_rgba.size, (0, 0, 0, 0))
    glow_img = Image.composite(tint, transparent, a)
    return glow_img.filter(ImageFilter.GaussianBlur(max(1, int(shape_rgba.size[0] * blur_ratio))))


def canvas_with_bg(size, c1, c2):
    """深空底 + 霓虹描边光晕 + 锐利描边 + 玻璃高光, 返回 RGBA 画布。"""
    mask = rounded_mask(size)
    canvas = Image.new("RGBA", (size, size))
    canvas.paste(radial_dark(size), (0, 0), mask)      # 深空底

    ring = neon_ring(size, c1, c2)
    # 外发光: 把描边环整体模糊一层垫在下面, 形成霓虹光晕
    halo = glow(ring, (min(c1[0], c2[0]), min(c1[1], c2[1]), min(c1[2], c2[2])), 0.028)
    canvas = Image.alpha_composite(canvas, halo)
    # 再补一层色彩更浓的光晕 (用较亮的端点色)
    halo2 = glow(ring, c2, 0.012)
    canvas = Image.alpha_composite(canvas, halo2)
    canvas = Image.alpha_composite(canvas, ring)       # 锐利描边在上层

    # 内描边 (白色半透明, 让边缘更有"发光管"层次)
    edge = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(edge)
    d.rounded_rectangle([0, 0, size - 1, size - 1],
                        radius=int(size * 0.22),
                        outline=(255, 255, 255, 70), width=max(1, size // 320))
    canvas = Image.alpha_composite(canvas, edge)
    return Image.alpha_composite(canvas, glass_overlay(size, mask))


def shift_args(args, dx, dy):
    """把坐标元组整体平移 (dx, dy), 标量参数 (如线宽) 保持不变。"""
    return [tuple(v + (dx if i % 2 == 0 else dy) for i, v in enumerate(p))
            if isinstance(p, tuple) else p for p in args]


def draw_glyph_with_glow(canvas, d, shapes, s, glow_color, shadow=True):
    """先画同色光晕 + 淡阴影, 再画白色主体 -> 霓虹灯管质感。
    shapes: [(方法名, 归一化坐标列表, {关键字参数}), ...], 坐标 0..1 按 s 放大。
    kwargs 支持 outline/width/radius/start/end: 若含 outline 则画"空心描边"形状
    (fill 默认为透明), 否则画白色实心形状。"""
    def norm_args(args, s):
        return [tuple(round(v * s) for v in p) if isinstance(p, tuple) else int(round(p * s))
                for p in args]

    def norm_kw(kwargs, s):
        return {k: (int(round(v * s)) if not isinstance(v, tuple)
                    else tuple(round(t * s) for t in v)) for k, v in kwargs.items()}

    # 光晕: 先在一张透明层上画纯白形状 (描边形状画白描边, 实心形状画白填充), 染成 glow_color 模糊后垫底
    ghost = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    gd = ImageDraw.Draw(ghost)
    for cmd, args, kwargs in shapes:
        norm = norm_args(args, s)
        kw = norm_kw(kwargs, s)
        if "outline" in kw:
            gkw = dict(kw); gkw.pop("fill", None)
            gkw["fill"] = (255, 255, 255, 255)
            gkw["outline"] = (255, 255, 255, 255)
            getattr(gd, cmd)(norm, **gkw)
        else:
            getattr(gd, cmd)(norm, **kw, fill=(255, 255, 255, 255))
    canvas = Image.alpha_composite(canvas, glow(ghost, glow_color, 0.02, strength=0.9))

    # 主体: 描边形状画白色描边 (内部透明, 露出深色底 -> 空心显示器框),
    # 实心形状画白色填充; 带一点右下偏移深色阴影增强立体感
    dx, dy = int(s * 0.008), int(s * 0.014)
    for cmd, args, kwargs in shapes:
        norm = norm_args(args, s)
        kw = norm_kw(kwargs, s)
        fn = getattr(d, cmd)
        if "outline" in kw:
            w = kw.get("width", 1)
            if shadow:
                fn(shift_args(norm, dx, dy), **dict(kw, outline=(10, 12, 30, 120)))
            kw["fill"] = (0, 0, 0, 0)      # 内部透明 -> 空心
            kw["outline"] = (255, 255, 255, 255)
            fn(norm, **kw)
        else:
            if shadow:
                fn(shift_args(norm, dx, dy), **kw, fill=(10, 12, 30, 120))
            fn(norm, **kw, fill=(255, 255, 255, 255))
    return canvas


# ---------------- 学生端: 显示器 + 播放 (观看) ----------------


def student_glyph(canvas, d, s):
    shapes = [
        # 显示器外框: 白色描边圆角矩形, 内部透明 (空心, 露出深色底)
        ("rounded_rectangle", [(0.315, 0.36), (0.685, 0.62)],
         {"radius": 0.03, "outline": (255, 255, 255, 255), "width": 0.045}),
        # 底座
        ("line", [(0.455, 0.62), (0.455, 0.68)], {"width": 0.035}),
        ("line", [(0.42, 0.68), (0.58, 0.68)], {"width": 0.032}),
        # 屏幕内的播放三角 (白色实心, 在深色屏上清晰可见)
        ("polygon", [(0.462, 0.415), (0.462, 0.565), (0.568, 0.49)], {}),
    ]
    return draw_glyph_with_glow(canvas, d, shapes, s, (56, 189, 248))


# ---------------- 教师端: 播放 + 广播波纹 (推流) ----------------


def teacher_glyph(canvas, d, s):
    shapes = [
        # 播放三角
        ("polygon", [(0.415, 0.36), (0.415, 0.64), (0.62, 0.50)], {}),
        # 右方广播波纹 (两段圆弧)
        ("arc", [(0.645, 0.36), (0.855, 0.64)], {"start": -50, "end": 50, "width": 0.045}),
        ("arc", [(0.73, 0.28), (0.94, 0.72)], {"start": -38, "end": 38, "width": 0.035}),
    ]
    return draw_glyph_with_glow(canvas, d, shapes, s, (249, 115, 22))


# ---------------- 主流程 ----------------


def build_icon(c1, c2, glyph_fn, out_path):
    canvas = canvas_with_bg(MASTER, c1, c2)
    d = ImageDraw.Draw(canvas)
    canvas = glyph_fn(canvas, d, MASTER)
    # 导出多尺寸 ICO (256 自动以 PNG 存储)
    canvas.resize((256, 256), Image.LANCZOS).save(
        out_path, format="ICO", sizes=[(sz, sz) for sz in SIZES])
    # PNG 预览
    canvas.resize((256, 256), Image.LANCZOS).save(out_path.replace(".ico", "_preview.png"))
    return canvas


if __name__ == "__main__":
    STUDENT = ((34, 211, 238), (99, 102, 241))        # 青 -> 靛蓝
    TEACHER = ((251, 146, 60), (236, 72, 153))        # 橙 -> 玫红

    print("生成 student.ico ...")
    c = build_icon(*STUDENT, student_glyph, "student.ico")
    print("生成 teacher.ico ...")
    build_icon(*TEACHER, teacher_glyph, "teacher.ico")

    # 合成对比预览图
    side = Image.new("RGBA", (512 + 24, 256), (12, 14, 28, 255))
    side.paste(c.resize((256, 256), Image.LANCZOS), (0, 0))
    t = Image.open("teacher_preview.png")
    side.paste(t, (256 + 24, 0))
    side.convert("RGB").save("preview.png")
    print("完成: student.ico / teacher.ico / preview.png")
