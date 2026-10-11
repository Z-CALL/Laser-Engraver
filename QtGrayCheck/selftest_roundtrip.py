#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
复刻 Widget::buildGrayArrayLines() 的导出，做回环自测。

链路（严格照抄上位机）：
   原图 → 缩放(w x h) → 灰度 Grayscale8 → 反相 + 低灰度归零(kGrayCutoff=20)
        → (可选)灰度映射 lo..hi → (可选)弓字形奇数行倒序 → 格式化成 C 数组文本

然后调用 GrayCheck.exe 反解，和原图逐像素比对。
"""
import os, subprocess, sys

CUTOFF = 20          # widget.cpp: static const int kGrayCutoff = 20
W = H = 200


def host_pipeline(src_gray, w, h, bidi, lo=0, hi=255):
    """把原图灰度走一遍上位机的变换，返回【数组值】(list of rows)。

    ★ 顺序必须严格照抄 widget.cpp::refreshGray() + buildGrayArrayLines()：
         v = 255 - src            ← 反相（黑=255）【这一步最容易漏！】
         if v < CUTOFF: v = 0     ← 低灰度截断（注意是反相【之后】判）
         若启用映射: v = lo + (v*(hi-lo)+127)/255
         若弓字形且奇数行: 整行倒序存
    """
    mapped = (lo != 0) or (hi != 255)
    rows = []
    for y in range(h):
        rev = bidi and (y & 1)
        row = []
        for x in range(w):
            sx = (w - 1 - x) if rev else x
            v = 255 - src_gray[y][sx]      # ← 反相：黑(0) -> 255，白(255) -> 0
            if v < CUTOFF:                 # ← 截断判的是反相后的值
                v = 0
            if mapped and v != 0:
                v = lo + (v * (hi - lo) + 127) // 255
                v = max(0, min(255, v))
            row.append(v)
        rows.append(row)
    return rows


def write_c_array(path, rows, w, h, bidi, lo=0, hi=255):
    """严格复刻 buildGrayArrayLines() 的文本格式"""
    mapped = (lo != 0) or (hi != 255)
    L = []
    L.append("/*")
    L.append(" * 灰度值二维数组(0~255) —— 由「激光雕刻上位机」导出")
    L.append(" *")
    L.append(" * 源图    : 自测生成")
    L.append(" * 尺寸    : %d x %d  (宽 %d 点/行, 高 %d 行)" % (w, h, w, h))
    L.append(" * 取值    : 0~255")
    L.append(" * 极性    : 黑=255, 白=0 (已反相, 固定)")
    L.append(" * 低灰度  : 灰度值 < %d 的已归零(去背景灰底)" % CUTOFF)
    L.append(" * 排列    : %s" % ("弓字形(奇数行倒序) —— 与运动顺序一致" if bidi
                                   else "自然行序(每行都从左到右)"))
    L.append(" * 灰度映射: %s" % ("%d -> %d (下限/上限)" % (lo, hi) if mapped else "无(原值 0~255)"))
    L.append(" * 行序    : gray_image[y][x]")
    L.append(" * 总点数  : %d = %d x %d" % (w * h, h, w))
    L.append(" */")
    L.append("")
    L.append("#define GRAY_IMG_W  %d" % w)
    L.append("#define GRAY_IMG_H  %d" % h)
    L.append("")
    L.append("const unsigned char gray_image[GRAY_IMG_H][GRAY_IMG_W] = {")
    for y in range(h):
        s = "    {"
        for x in range(w):
            s += "%3d" % rows[y][x]
            if x == w - 1:
                break
            s += ","
            s += "\n     " if (x + 1) % 16 == 0 else " "
        s += " }" if y == h - 1 else " },"
        s += "   /* y=%d %s */" % (y, "<-" if (bidi and (y & 1)) else "->")
        L.append(s)
    L.append("};")
    open(path, "w", encoding="utf-8").write("\n".join(L) + "\n")


def make_source(kind, w, h):
    """造几张有辨识度的原图（0=黑 255=白，注意这是原图灰度，不是数组值）"""
    g = [[255] * w for _ in range(h)]
    if kind == "arrow":
        # 指向右上角的箭头：黑
        for y in range(h):
            for x in range(w):
                # 箭杆：45 度带宽
                if abs((x - (w - 1 - y))) <= 6:
                    g[y][x] = 0
        # 箭头：右上角三角
        for y in range(int(h * 0.22)):
            for x in range(w - 1 - y * 2, w):
                if x >= 0:
                    g[y][x] = 0
    elif kind == "grad_h":
        for y in range(h):
            for x in range(w):
                g[y][x] = int(255.0 * x / (w - 1))
    elif kind == "text_F":
        for y in range(int(h * 0.15), int(h * 0.85)):
            for x in range(int(w * 0.12), int(w * 0.22)):
                g[y][x] = 0
        for y in range(int(h * 0.15), int(h * 0.28)):
            for x in range(int(w * 0.22), int(w * 0.55)):
                g[y][x] = 0
        for y in range(int(h * 0.45), int(h * 0.58)):
            for x in range(int(w * 0.22), int(w * 0.45)):
                g[y][x] = 0
    return g


def check(tag, kind, bidi, lo=0, hi=255):
    src = make_source(kind, W, H)
    rows = host_pipeline(src, W, H, bidi, lo, hi)
    hp = os.path.join(TMP, "export_%s.h" % tag)
    op = os.path.join(TMP, "restored_%s.png" % tag)
    write_c_array(hp, rows, W, H, bidi, lo, hi)

    exe = os.path.join(TMP, "release", "GrayCheck.exe")
    p = subprocess.run([exe, hp, op], capture_output=True, text=True)
    ok = (p.returncode == 0)
    print("=" * 78)
    print("[%s]  原图=%s  弓字形=%s  映射=%s->%s" % (tag, kind, bidi, lo, hi))
    print("  GrayCheck 退出码: %d" % p.returncode)
    for line in (p.stdout or "").strip().splitlines():
        print("   " + line)
    if p.stderr.strip():
        print("   STDERR: " + p.stderr.strip())

    if not ok or not os.path.exists(op):
        print("  >>> 结果: FAIL（没能产出还原图）")
        return False

    # 用 PIL 读回还原图，和"期望的原图观感"逐像素比
    try:
        from PIL import Image
    except ImportError:
        print("  (没有 PIL，跳过逐像素比对，只看上面的结构自检)")
        return ok

    im = Image.open(op).convert("L")
    px = im.load()
    iw, ih = im.size
    if (iw, ih) != (W, H):
        print("  >>> 尺寸不对: %dx%d" % (iw, ih))
        return False

    # 期望：还原图 = 原图，但要按上位机实际做过的变换算期望值。
    # ★ 截断判的是**反相后的值**（源码: val = 255 - src; if (val < CUTOFF) val = 0;）
    #   所以"会被归零"的像素是 src > 255-CUTOFF（接近纯白的那一小撮），
    #   不是 src < CUTOFF —— 这一点我第一版写反了，害得斜坡图误报 FAIL。
    mapped = (lo != 0) or (hi != 255)
    clipThreshold = 255 - CUTOFF          # = 235，src 高于它的像素会被归零
    diff = 0
    worst = 0
    worst_where = None
    for y in range(H):
        for x in range(W):
            sv = src[y][x]
            if sv > clipThreshold:
                # 反相后 < CUTOFF → 被归零 → 数组里是 0 → 还原成纯白
                exp = 255
                tol = 6
            else:
                exp = sv
                if mapped:
                    # 走一遍映射+逆映射，算真实可达误差
                    v = 255 - exp
                    if v >= CUTOFF:
                        v = lo + (v * (hi - lo) + 127) // 255
                        v = max(0, min(255, v))
                    back = 0 if v == 0 else int(round((v - lo) * 255.0 / (hi - lo)))
                    back = max(0, min(255, back))
                    exp = 255 - back
                    tol = 6
                else:
                    tol = 4
            got = px[x, y]
            d = abs(got - exp)
            if d > worst:
                worst = d
                worst_where = (x, y, sv, exp, got)
            if d > tol:
                diff += 1
    pct = 100.0 * diff / (W * H)
    print("  逐像素比对: 超差 %d / %d  (%.2f%%)   最大偏差 %d" % (diff, W * H, pct, worst))
    if diff and worst_where:
        print("     最差点 (x=%d,y=%d) 原图%d 期望%d 实得%d" % worst_where)
    good = (pct < 0.5)
    print("  >>> 结果: %s" % ("PASS 还原正确" if good else "FAIL 还原不一致"))
    return good


if __name__ == "__main__":
    TMP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\27594\Desktop\Laser_1\Laser-Engraver\.graycheck-build"
    print("工作目录: %s\n" % TMP)
    results = []
    results.append(check("arrow_nobidi", "arrow",   False))
    results.append(check("arrow_bidi",   "arrow",   True))
    results.append(check("grah_nobidi",  "grad_h",  False))
    results.append(check("grah_bidi",    "grad_h",  True))
    results.append(check("textF_bidi",   "text_F",  True))
    results.append(check("textF_map",    "text_F",  True, lo=40, hi=200))
    print("\n" + "=" * 78)
    print("总计: %d/%d PASS" % (sum(1 for r in results if r), len(results)))
