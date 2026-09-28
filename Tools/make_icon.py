#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成上位机图标 app.ico：白色圆角底 + 浅灰网格（对应曲线区）+
红色电压曲线 + 绿色电池，与程序界面配色一致。

运行：.venv/Scripts/python.exe make_icon.py
输出：app.ico（含 256/128/64/48/32/16 各尺寸）
"""

from pathlib import Path

from PIL import Image, ImageDraw

S = 2048  # 超采样画布，缩小后边缘平滑

RED = (200, 30, 30, 255)      # 与曲线画笔一致
GREEN = (26, 127, 55, 255)    # 与按钮/数值一致
BG = (250, 250, 252, 255)
BORDER = (217, 220, 227, 255)
GRID = (228, 231, 236, 255)


def main():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    m = int(S * 0.04)
    d.rounded_rectangle(
        [m, m, S - m, S - m], radius=int(S * 0.18),
        fill=BG, outline=BORDER, width=int(S * 0.012),
    )

    # 网格
    gw = max(int(S * 0.006), 2)
    for i in range(1, 6):
        x = m + (S - 2 * m) * i // 6
        d.line([x, m, x, S - m], fill=GRID, width=gw)
        y = m + (S - 2 * m) * i // 6
        d.line([m, y, S - m, y], fill=GRID, width=gw)

    # 红色电压曲线（抬升的充电曲线形状，带轻微抖动）
    pts = [
        (0.07, 0.72), (0.14, 0.70), (0.20, 0.735), (0.27, 0.66), (0.33, 0.68),
        (0.40, 0.58), (0.47, 0.62), (0.54, 0.47), (0.60, 0.52), (0.68, 0.34),
        (0.75, 0.38), (0.83, 0.27), (0.92, 0.23),
    ]
    px = [(m + (S - 2 * m) * x, m + (S - 2 * m) * y) for x, y in pts]
    d.line(px, fill=RED, width=int(S * 0.045), joint="curve")
    # 曲线端点小圆点
    r = int(S * 0.028)
    for p in (px[0], px[-1]):
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=RED)

    # 右上角绿色电池（电量约 60%）
    bx0, by0, bx1, by1 = S * 0.60, S * 0.08, S * 0.86, S * 0.20
    rr = S * 0.02
    d.rounded_rectangle(
        [bx0, by0, bx1, by1], radius=rr,
        outline=GREEN, width=max(int(S * 0.013), 2),
    )
    d.rounded_rectangle(
        [bx0 + S * 0.011, by0 + S * 0.011,
         bx0 + (bx1 - bx0) * 0.62, by1 - S * 0.011],
        radius=rr * 0.6, fill=GREEN,
    )
    d.rounded_rectangle(
        [bx1, by0 + (by1 - by0) * 0.3, bx1 + S * 0.032, by0 + (by1 - by0) * 0.7],
        radius=S * 0.012, fill=GREEN,
    )

    out = Path(__file__).resolve().parent / "app.ico"
    img.resize((256, 256), Image.LANCZOS).save(
        out, sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
    )
    print(f"OK {out}")


if __name__ == "__main__":
    main()
