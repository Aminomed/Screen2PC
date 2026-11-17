#!/usr/bin/env python3
"""
Deterministic generator for Screen2PC's transparent multi-resolution Windows icon
(Konzept 2: "Dual-Glass Projection").

Generates `src/common/assets/Screen2PC.ico` containing:
  - 16x16, 20x20, 24x24, 32x32, 48x48, 64x64: 32-bit BGRA DIB BMP + 1-bpp AND mask
  - 256x256: PNG payload (Windows Vista/10/11 standard)
"""

import os
import struct
from io import BytesIO
import numpy as np
from PIL import Image


def sdf_rounded_box(px, py, cx, cy, hx, hy, r):
    dx = np.abs(px - cx) - (hx - r)
    dy = np.abs(py - cy) - (hy - r)
    qx = np.maximum(dx, 0.0)
    qy = np.maximum(dy, 0.0)
    outside = np.hypot(qx, qy)
    inside = np.minimum(np.maximum(dx, dy), 0.0)
    return outside + inside - r


def sdf_convex_quad(px, py, pts):
    """Exact Euclidean SDF to a convex quad given [(x,y) x4]."""
    n = len(pts)
    min_d2 = np.full_like(px, np.inf, dtype=np.float32)
    area = 0.0
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        area += x0 * y1 - x1 * y0
    ordered = list(pts) if area > 0 else list(reversed(pts))

    inside = np.ones_like(px, dtype=bool)
    for i in range(n):
        ax, ay = ordered[i]
        bx, by = ordered[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        wx, wy = px - ax, py - ay
        cross = ex * wy - ey * wx
        inside &= (cross >= 0.0)

        len2 = ex * ex + ey * ey
        t = np.clip((wx * ex + wy * ey) / len2, 0.0, 1.0)
        dx = wx - ex * t
        dy = wy - ey * t
        d2 = dx * dx + dy * dy
        min_d2 = np.minimum(min_d2, d2)

    d = np.sqrt(min_d2)
    return np.where(inside, -d, d)


def sdf_rounded_quad(px, py, pts, r):
    return sdf_convex_quad(px, py, pts) - r


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def coverage(sdf, aa):
    return np.clip(0.5 - sdf / aa, 0.0, 1.0)


def over(dst_rgba, src_rgb, src_a):
    src_a = np.clip(src_a, 0.0, 1.0)[..., None]
    dst_rgb = dst_rgba[..., :3]
    dst_a = dst_rgba[..., 3:4]

    out_a = src_a + dst_a * (1.0 - src_a)
    safe_a = np.maximum(out_a, 1e-6)
    out_rgb = (src_rgb * src_a + dst_rgb * dst_a * (1.0 - src_a)) / safe_a
    out_rgb = np.where(out_a > 1e-6, out_rgb, 0.0)
    return np.concatenate([out_rgb, out_a], axis=-1)


def render_icon(size):
    """Render Dual-Glass Projection monitor icon at exact pixel size `size`."""
    ss = 8 if size <= 48 else 4
    w = size * ss
    coords = (np.arange(w, dtype=np.float32) + 0.5) / w
    px, py = np.meshgrid(coords, coords)
    aa = (0.58 if size <= 24 else 0.78) / size

    canvas = np.zeros((w, w, 4), dtype=np.float32)

    if size <= 20:
        px_unit = 1.0 / size
        bezel_cx = 0.50
        bezel_cy = round(0.40 * size) / size
        bezel_hx = round(0.435 * size) / size
        bezel_hy = round(0.285 * size) / size
        frame_stroke = (1.65 if size == 16 else 1.85) * px_unit
        frame_r_out = 2.4 * px_unit
        neck_hx = (1.6 if size == 16 else 1.8) * px_unit
        neck_stroke = 0.0
        hollow_neck = False
        base_hx = round(0.21 * size) / size
        base_hy = (1.05 if size == 16 else 1.15) * px_unit
        base_cy = bezel_cy + bezel_hy + (2.35 if size == 16 else 2.5) * px_unit
        pane_scale = 1.08
        pane_cx_shift = 0.008
        pane_cy_shift = -0.004
        pane_border_w = 0.032
        pane_r = 0.034
        draw_echo = False
    elif size <= 32:
        bezel_cx = 0.50
        bezel_cy = 0.405
        bezel_hx = 0.415
        bezel_hy = 0.282
        frame_stroke = 0.058
        frame_r_out = 0.082
        neck_hx = 0.072
        neck_stroke = 0.044
        hollow_neck = (size >= 32)
        base_hx = 0.195
        base_hy = 0.031
        base_cy = 0.836
        pane_scale = 0.98
        pane_cx_shift = 0.012
        pane_cy_shift = -0.006
        pane_border_w = 0.022
        pane_r = 0.036
        draw_echo = (size >= 32)
    else:
        bezel_cx = 0.50
        bezel_cy = 0.405
        bezel_hx = 0.408
        bezel_hy = 0.278
        frame_stroke = 0.050
        frame_r_out = 0.082
        neck_hx = 0.068
        neck_stroke = 0.042
        hollow_neck = True
        base_hx = 0.185
        base_hy = 0.029
        base_cy = 0.835
        pane_scale = 0.94
        pane_cx_shift = 0.016
        pane_cy_shift = -0.008
        pane_border_w = 0.015
        pane_r = 0.036
        draw_echo = True

    # 1. Unified Monitor Bezel + Stand Silhouette (seam-free boolean union)
    sdf_bezel_outer = sdf_rounded_box(px, py, bezel_cx, bezel_cy, bezel_hx, bezel_hy, frame_r_out)
    inner_hx = bezel_hx - frame_stroke
    inner_hy = bezel_hy - frame_stroke
    inner_r = max(frame_r_out - frame_stroke * 0.72, 0.015)
    sdf_bezel_inner = sdf_rounded_box(px, py, bezel_cx, bezel_cy, inner_hx, inner_hy, inner_r)
    sdf_bezel_ring = np.maximum(sdf_bezel_outer, -sdf_bezel_inner)

    neck_top_in = bezel_cy + bezel_hy - frame_stroke * 0.55
    neck_bot_in = base_cy + base_hy * 0.2
    neck_cy = 0.5 * (neck_top_in + neck_bot_in)
    neck_hy = 0.5 * (neck_bot_in - neck_top_in)
    sdf_neck_solid = sdf_rounded_box(px, py, bezel_cx, neck_cy, neck_hx, neck_hy, 0.008)
    if hollow_neck:
        hole_hx = max(neck_hx - neck_stroke, 0.010)
        hole_top = bezel_cy + bezel_hy - 0.004
        hole_bot = base_cy - base_hy + 0.004
        hole_cy = 0.5 * (hole_top + hole_bot)
        hole_hy = 0.5 * (hole_bot - hole_top)
        sdf_neck_hole = sdf_rounded_box(px, py, bezel_cx, hole_cy, hole_hx, hole_hy, 0.004)
        sdf_neck = np.maximum(sdf_neck_solid, -sdf_neck_hole)
    else:
        sdf_neck = sdf_neck_solid

    sdf_base = sdf_rounded_box(px, py, bezel_cx, base_cy, base_hx, base_hy, base_hy)
    sdf_frame = np.minimum(sdf_bezel_ring, np.minimum(sdf_neck, sdf_base))

    # 2. Dark Smoked Screen Fill behind Bezel
    sdf_screen_fill = sdf_rounded_box(
        px, py, bezel_cx, bezel_cy, inner_hx + frame_stroke * 0.35, inner_hy + frame_stroke * 0.35, inner_r
    )
    screen_a = coverage(sdf_screen_fill, aa) * 0.92
    vt = np.clip((py - (bezel_cy - inner_hy)) / (2.0 * inner_hy), 0.0, 1.0)
    screen_rgb = np.stack([
        0.058 + 0.014 * (1.0 - vt),
        0.078 + 0.018 * (1.0 - vt),
        0.118 + 0.024 * (1.0 - vt),
    ], axis=-1)
    canvas = over(canvas, screen_rgb, screen_a)

    # 3. Sleek Slate Metallic Monitor Frame (high legibility on Light & Dark Mode)
    frame_a = coverage(sdf_frame, aa)
    fy = np.clip((py - (bezel_cy - bezel_hy)) / (base_cy + base_hy - (bezel_cy - bezel_hy)), 0.0, 1.0)
    frame_rgb = np.stack([
        0.465 - 0.125 * fy,
        0.520 - 0.130 * fy,
        0.595 - 0.135 * fy,
    ], axis=-1)
    top_edge_band = np.exp(-np.maximum(-sdf_frame, 0.0) / (0.009 if size > 24 else 0.014))
    top_facing = smoothstep(0.65, 0.0, fy)
    frame_rgb = np.clip(frame_rgb + 0.08 * top_edge_band[..., None] * top_facing[..., None], 0.0, 1.0)
    canvas = over(canvas, frame_rgb, frame_a)

    # 4. Dual Overlapping Floating Glass Panes
    def make_pane_quad(cx, cy, half_w, half_h, skew_x, tilt_y, flare_right=0.010):
        tl = (cx - half_w + skew_x, cy - half_h + tilt_y * 0.5)
        tr = (cx + half_w + skew_x, cy - half_h - tilt_y * 0.7 - flare_right * 0.2)
        br = (cx + half_w - skew_x, cy + half_h + tilt_y * 0.3 + flare_right)
        bl = (cx - half_w - skew_x, cy + half_h - tilt_y * 0.3)
        return [tl, tr, br, bl]

    center_x = bezel_cx + pane_cx_shift
    center_y = bezel_cy + pane_cy_shift

    back_pts = make_pane_quad(
        cx=center_x + 0.036 * pane_scale,
        cy=center_y + 0.034 * pane_scale,
        half_w=0.140 * pane_scale,
        half_h=0.100 * pane_scale,
        skew_x=0.038 * pane_scale,
        tilt_y=0.016 * pane_scale,
        flare_right=0.010 * pane_scale,
    )
    sdf_back = sdf_rounded_quad(px, py, back_pts, pane_r * pane_scale)

    front_pts = make_pane_quad(
        cx=center_x - 0.020 * pane_scale,
        cy=center_y - 0.018 * pane_scale,
        half_w=0.145 * pane_scale,
        half_h=0.104 * pane_scale,
        skew_x=0.042 * pane_scale,
        tilt_y=0.018 * pane_scale,
        flare_right=0.010 * pane_scale,
    )
    sdf_front = sdf_rounded_quad(px, py, front_pts, pane_r * pane_scale)

    if draw_echo:
        echo_pts = make_pane_quad(
            cx=center_x + 0.060 * pane_scale,
            cy=center_y + 0.058 * pane_scale,
            half_w=0.138 * pane_scale,
            half_h=0.098 * pane_scale,
            skew_x=0.036 * pane_scale,
            tilt_y=0.016 * pane_scale,
            flare_right=0.010 * pane_scale,
        )
        sdf_echo = sdf_rounded_quad(px, py, echo_pts, pane_r * pane_scale)
        echo_ring = np.abs(sdf_echo) - (pane_border_w * 0.36)
        echo_mask = coverage(echo_ring, aa) * smoothstep(0.004, 0.022, sdf_back)
        echo_mask *= smoothstep(0.010, -0.010, sdf_bezel_inner)
        diag = np.clip((px - (center_x - 0.04)) * 2.8 + (py - (center_y - 0.01)) * 2.8, 0.0, 1.0)
        echo_rgb = np.stack([
            0.08 + 0.14 * diag,
            0.55 + 0.25 * diag,
            0.98 * np.ones_like(px),
        ], axis=-1)
        canvas = over(canvas, echo_rgb, echo_mask * 0.72 * diag)

    # Inner screen cyan glow confined strictly inside bezel cutout
    glow_dist = np.maximum(np.minimum(sdf_back, sdf_front), 0.0)
    aura = np.exp(-glow_dist * glow_dist / (0.042 ** 2)) * 0.32
    aura *= smoothstep(0.008, -0.015, sdf_bezel_inner)
    aura_rgb = np.stack([
        0.04 * np.ones_like(px),
        0.60 * np.ones_like(px),
        1.00 * np.ones_like(px),
    ], axis=-1)
    canvas = over(canvas, aura_rgb, aura)

    # Back Glass Pane
    back_fill_a = coverage(sdf_back, aa) * 0.82
    gu_b = np.clip((px - (center_x - 0.13)) / 0.32, 0.0, 1.0)
    gv_b = np.clip((py - (center_y - 0.09)) / 0.22, 0.0, 1.0)
    gt_b = 0.55 * gu_b + 0.45 * gv_b
    back_rgb = np.stack([
        0.04 + 0.08 * (1.0 - gt_b),
        0.24 + 0.22 * (1.0 - gt_b),
        0.74 + 0.22 * gt_b,
    ], axis=-1)
    canvas = over(canvas, back_rgb, back_fill_a)

    back_rim_sdf = np.abs(sdf_back + pane_border_w * 0.22) - pane_border_w * 0.42
    back_rim_a = coverage(back_rim_sdf, aa) * 0.92
    back_rim_rgb = np.stack([
        0.16 + 0.14 * (1.0 - gt_b),
        0.66 + 0.22 * (1.0 - gt_b),
        1.00 * np.ones_like(px),
    ], axis=-1)
    canvas = over(canvas, back_rim_rgb, back_rim_a)

    # Front Glass Pane
    front_fill_a = coverage(sdf_front, aa) * (0.86 if size > 24 else 0.92)
    gu_f = np.clip((px - (center_x - 0.18)) / 0.32, 0.0, 1.0)
    gv_f = np.clip((py - (center_y - 0.12)) / 0.22, 0.0, 1.0)
    gt_f = np.clip(0.62 * gu_f + 0.38 * gv_f, 0.0, 1.0)

    t1 = np.clip(gt_f / 0.48, 0.0, 1.0)
    t2 = np.clip((gt_f - 0.48) / 0.52, 0.0, 1.0)
    r_f = np.where(gt_f < 0.48, 0.15 * (1 - t1) + 0.07 * t1, 0.07 * (1 - t2) + 0.12 * t2)
    g_f = np.where(gt_f < 0.48, 1.00 * (1 - t1) + 0.78 * t1, 0.78 * (1 - t2) + 0.36 * t2)
    b_f = np.where(gt_f < 0.48, 0.84 * (1 - t1) + 1.00 * t1, 1.00 * (1 - t2) + 1.00 * t2)

    rel_y = (py - (center_y - 0.018 * pane_scale)) / (0.104 * pane_scale)
    rel_x = (px - (center_x - 0.020 * pane_scale)) / (0.145 * pane_scale)
    curve_y = -0.34 - 0.14 * rel_x + 0.08 * (rel_x * rel_x)
    top_gleam = smoothstep(curve_y + 0.08, curve_y - 0.10, rel_y)
    gleam_amt = 0.24 if size > 24 else 0.18
    r_f = np.clip(r_f + gleam_amt * top_gleam, 0.0, 1.0)
    g_f = np.clip(g_f + gleam_amt * top_gleam * 0.92, 0.0, 1.0)
    b_f = np.clip(b_f + gleam_amt * top_gleam * 0.55, 0.0, 1.0)

    front_rgb = np.stack([r_f, g_f, b_f], axis=-1)
    canvas = over(canvas, front_rgb, front_fill_a)

    front_rim_sdf = np.abs(sdf_front + pane_border_w * 0.22) - pane_border_w * 0.48
    front_rim_a = coverage(front_rim_sdf, aa) * 0.98
    rim_rgb = np.clip(np.stack([
        0.50 + 0.12 * (1.0 - gt_f) - 0.26 * gt_f,
        0.98 - 0.18 * gt_f,
        0.99 * np.ones_like(px),
    ], axis=-1), 0.0, 1.0)
    canvas = over(canvas, rim_rgb, front_rim_a)

    # Premultiplied downsample -> clean unpremultiplied RGBA
    rgb_pm = canvas[..., :3] * canvas[..., 3:4]
    rgba_pm = np.concatenate([rgb_pm, canvas[..., 3:4]], axis=-1)
    rgba_ds = rgba_pm.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))

    out_a = np.clip(rgba_ds[..., 3:4], 0.0, 1.0)
    safe_a = np.maximum(out_a, 1e-6)
    out_rgb = np.where(out_a > 1e-6, rgba_ds[..., :3] / safe_a, 0.0)
    out_rgba = np.clip(np.concatenate([out_rgb, out_a], axis=-1) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return Image.fromarray(out_rgba, mode="RGBA")


def save_windows_ico(images_by_size, out_path):
    """Write Windows-compliant multi-resolution .ico (DIB <= 64px + PNG 256px)."""
    entries = []
    for sz in sorted(images_by_size.keys()):
        im = images_by_size[sz].convert("RGBA")
        w, h = im.size
        if w >= 256:
            bio = BytesIO()
            im.save(bio, format="PNG", optimize=True)
            payload = bio.getvalue()
        else:
            arr = np.array(im, dtype=np.uint8)
            arr_bu = arr[::-1, :, :]
            bgra = arr_bu[:, :, [2, 1, 0, 3]].tobytes()

            row_stride = ((w + 31) // 32) * 4
            and_bytes = bytearray(row_stride * h)
            alpha_bu = arr_bu[:, :, 3]
            for y in range(h):
                for x in range(w):
                    if alpha_bu[y, x] < 16:
                        and_bytes[y * row_stride + (x >> 3)] |= (0x80 >> (x & 7))

            bmp_header = struct.pack(
                "<IIIHHIIIIII",
                40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0,
            )
            payload = bmp_header + bgra + bytes(and_bytes)

        entries.append((w, h, payload))

    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    dir_bytes = bytearray()
    blob_bytes = bytearray()
    for w, h, payload in entries:
        dir_bytes += struct.pack(
            "<BBBBHHII",
            0 if w >= 256 else w,
            0 if h >= 256 else h,
            0, 0, 1, 32,
            len(payload), offset,
        )
        blob_bytes += payload
        offset += len(payload)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(dir_bytes)
        f.write(blob_bytes)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_ico = os.path.join(repo_root, "src", "common", "assets", "Screen2PC.ico")
    sizes = [16, 20, 24, 32, 48, 64, 256]
    imgs = {sz: render_icon(sz) for sz in sizes}
    save_windows_ico(imgs, out_ico)

    loaded = Image.open(out_ico)
    print(f"Generated {out_ico} ({os.path.getsize(out_ico)} bytes), sizes={sorted(loaded.ico.sizes())}")


if __name__ == "__main__":
    main()
