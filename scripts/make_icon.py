# -*- coding: utf-8 -*-
"""
make_icon.py —— 把一张 1024x1024 的原始 PNG 加工成 Windows 应用图标资源
--------------------------------------------------------------------------
做三件事：
1) 把方形原图裁成"圆角方形"（透明度由圆角外区域挖空），符合 Win11 图标观感；
2) 生成 Qt 运行时用到的 256x256 PNG（放进 qrc，用于窗口/任务栏图标）；
3) 生成多尺寸 app.ico（16/24/32/40/48/64/128/256），供 MSVC 资源编译器嵌入 exe。
用法：python scripts/make_icon.py
"""
from PIL import Image, ImageDraw

SRC = r"D:\dev-project\Cleveil-1.0.0\resources\icon-1024.png"
OUT_ICO = r"D:\dev-project\Cleveil-1.0.0\resources\app.ico"
OUT_PNG = r"D:\dev-project\Cleveil-1.0.0\resources\icon-256.png"

SIZE = 1024

src = Image.open(SRC).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)

# ---- 1. 圆角遮罩：半径取 18%，和 Windows 11 应用图标一致 ----
radius = int(SIZE * 0.18)
mask = Image.new("L", (SIZE, SIZE), 0)
draw = ImageDraw.Draw(mask)
draw.rounded_rectangle([0, 0, SIZE - 1, SIZE - 1], radius=radius, fill=255)
src.putalpha(mask)

# ---- 2. 运行时 PNG（256 是 Windows 大图标的标准尺寸）----
src.resize((256, 256), Image.LANCZOS).save(OUT_PNG, "PNG")

# ---- 3. 多尺寸 ICO ----
# Pillow 会为每个尺寸单独做一次高质量重采样，小尺寸下箭头依然可辨
sizes = [(16, 16), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)]
src.save(OUT_ICO, format="ICO", sizes=sizes)

print("icon-256.png:", Image.open(OUT_PNG).size)
print("app.ico:", Image.open(OUT_ICO).size, "entries:", len(sizes))
