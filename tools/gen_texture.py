#!/usr/bin/env python3
"""Compile-time texture generator for TinyGL on the Xiaomiao ESP32.

Produces C headers containing `const GLubyte` RGB texture arrays, ready for
`glTexImage2D(GL_TEXTURE_2D, 0, 3, W, H, 0, GL_RGB, GL_UNSIGNED_BYTE, ...)`.
Headers are emitted into the build directory and #included by tinygl_test.c,
so textures live in flash (.rodata) instead of being generated at runtime.

Textures are generated programmatically:
  - ceramic:  warm beige with gradient, speckle, dark rim, warm band
  - checker:  8x8 red/white checkerboard (classic UV-debug pattern)
  - brick:    running-bond brick wall (red mortar, grey bricks)
  - grid:     white grid on dark blue (coordinate reference)

Usage:
    gen_texture.py <kind> <output_header.h> [texture_name] [width] [height]

Defaults: width=height=256 (TinyGL's TGL_FEATURE_TEXTURE_DIM; non-256 source
would be resampled at upload).
"""
import sys
import os

# Default ceramic palette — warm beige with subtle vertical gradient, speckle
# noise, a darkened rim band and a warm middle band.
BASE_R = 210
BASE_G = 180
BASE_B = 140


def gen_ceramic(width: int, height: int) -> bytearray:
    """Return width*height*3 RGB bytes for the ceramic texture."""
    buf = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 3
            br = BASE_R + (y * 30 // height)
            bg = BASE_G + (y * 35 // height)
            bb = BASE_B + (y * 40 // height)
            n = ((x * 17 + y * 31) & 15) - 8
            r = max(0, min(255, br + n))
            g = max(0, min(255, bg + n))
            b = max(0, min(255, bb + n))
            if y < (height // 8):
                r = r * 7 // 10
                g = g * 7 // 10
                b = b * 7 // 10
            band_lo = (height * 100) // 256
            band_hi = (height * 120) // 256
            if band_lo <= y < band_hi:
                r = min(255, r + 20)
                g = min(255, g + 15)
                b = min(255, b + 10)
            buf[i + 0] = r
            buf[i + 1] = g
            buf[i + 2] = b
    return buf


def gen_checker(width: int, height: int) -> bytearray:
    """8x8 red/white checkerboard — classic UV-debug pattern."""
    buf = bytearray(width * height * 3)
    cells = 8
    cw = width // cells
    ch = height // cells
    for y in range(height):
        for x in range(width):
            cx = x // cw
            cy = y // ch
            if (cx + cy) & 1:
                r, g, b = 220, 40, 40   # red
            else:
                r, g, b = 235, 235, 235  # white
            i = (y * width + x) * 3
            buf[i] = r; buf[i+1] = g; buf[i+2] = b
    return buf


def gen_brick(width: int, height: int) -> bytearray:
    """Running-bond brick wall: grey bricks, red mortar joints."""
    buf = bytearray(width * height * 3)
    rows = 8
    bh = height // rows
    bw = width // 4
    for y in range(height):
        row = y // bh
        offset = (bw // 2) if (row & 1) else 0
        for x in range(width):
            # brick local coords
            bx = (x + offset) % bw
            by = y % bh
            # mortar near edges (2px)
            if bx < 2 or by < 2:
                r, g, b = 90, 40, 30      # red mortar
            else:
                # brick body with slight color variation
                v = ((bx * 7 + by * 13) & 7) - 3
                r = 150 + v; g = 130 + v; b = 120 + v
            i = (y * width + x) * 3
            buf[i] = r; buf[i+1] = g; buf[i+2] = b
    return buf


def gen_grid(width: int, height: int) -> bytearray:
    """White grid on dark blue — coordinate reference texture."""
    buf = bytearray(width * height * 3)
    cells = 8
    cw = width // cells
    ch = height // cells
    for y in range(height):
        for x in range(width):
            if x % cw < 2 or y % ch < 2:
                r, g, b = 230, 230, 230  # grid lines
            else:
                r, g, b = 20, 30, 70     # dark blue
            i = (y * width + x) * 3
            buf[i] = r; buf[i+1] = g; buf[i+2] = b
    return buf


def gen_sky(width: int, height: int) -> bytearray:
    """Clear blue sky: zenith-deep to horizon-light gradient with soft white clouds.

    Used for the skybox top (+Y) face."""
    buf = bytearray(width * height * 3)
    clouds = [
        (0.22, 0.30, 0.16, 0.22),
        (0.60, 0.18, 0.20, 0.14),
        (0.45, 0.55, 0.24, 0.16),
        (0.80, 0.40, 0.14, 0.18),
        (0.10, 0.70, 0.16, 0.12),
        (0.38, 0.78, 0.18, 0.12),
    ]
    for y in range(height):
        t = y / height
        r = 70 + 60 * t
        g = 120 + 60 * t
        b = 190 + 40 * t
        for x in range(width):
            u = x / width
            cloud = 0.0
            for (cx, cy, rx, ry) in clouds:
                dx = (u - cx) / rx
                dy = (t - cy) / ry
                d2 = dx * dx + dy * dy
                if d2 < 1.0:
                    cloud += (1.0 - d2) * (1.0 - d2)
            if cloud > 1.0:
                cloud = 1.0
            cr = r + (255 - r) * cloud
            cg = g + (255 - g) * cloud
            cb = b + (255 - b) * cloud
            i = (y * width + x) * 3
            buf[i] = int(cr)
            buf[i + 1] = int(cg)
            buf[i + 2] = int(cb)
    return buf


def gen_sand(width: int, height: int) -> bytearray:
    """Desert sand: warm tan with subtle horizontal dune ripples and speckle noise.

    Used for the skybox bottom (-Y) face."""
    buf = bytearray(width * height * 3)
    base_r, base_g, base_b = 214, 182, 132
    for y in range(height):
        shade = 1.0 - 0.12 * (y / height)
        ripple = ((y * 7) // 16) & 1
        for x in range(width):
            n = ((x * 13 + y * 29) & 15) - 8
            r = int((base_r + ripple * 8 + n) * shade)
            g = int((base_g + ripple * 6 + n) * shade)
            b = int((base_b + ripple * 4 + n) * shade)
            i = (y * width + x) * 3
            buf[i] = max(0, min(255, r))
            buf[i + 1] = max(0, min(255, g))
            buf[i + 2] = max(0, min(255, b))
    return buf


def gen_metal(width: int, height: int) -> bytearray:
    """Metallic base texture with baked surface detail (bumps/scratches/pits).

    Combines Blinn-Phong specular lighting with a procedural heightfield
    so the bump relief is baked directly into the base texture — no extra
    texture unit consumed."""
    import math, random as _random
    rng = _random.Random(42)  # deterministic

    buf = bytearray(width * height * 3)
    cx, cy = width / 2.0, height / 2.0

    # Light direction (above-left-front, normalized)
    lx, ly, lz = -0.4, -0.5, 0.77
    ll = math.sqrt(lx*lx + ly*ly + lz*lz)
    lx /= ll; ly /= ll; lz /= ll

    # Viewer at normal incidence. Half-vector H = normalize(V + L)
    hx, hy, hz = lx, ly, lz + 1.0
    hl = math.sqrt(hx*hx + hy*hy + hz*hz)
    hx /= hl; hy /= hl; hz /= hl
    shininess = 64.0

    # ── Procedural heightfield features ────────────────────
    # Fine grain noise (sand-cast metal)
    nw, nh = 64, 64
    noise = [rng.randint(0, 255) for _ in range(nw * nh)]

    # Directional scratches (brushed metal)
    scratches = []
    for _ in range(8):
        scratches.append((
            rng.uniform(0, width), rng.uniform(0, height),
            rng.uniform(-0.6, 0.6),   # angle
            rng.uniform(60, 180),      # length
            rng.uniform(1.5, 3.5),     # width
        ))

    # Pits (casting defects)
    pits = []
    for _ in range(25):
        pits.append((
            rng.uniform(0, width), rng.uniform(0, height),
            rng.uniform(3.0, 10.0),   # radius
            rng.uniform(0.35, 0.75),   # depth
        ))

    # Rivets/studs
    rivets = []
    for _ in range(6):
        rivets.append((
            rng.uniform(0.12 * width, 0.88 * width),
            rng.uniform(0.12 * height, 0.88 * height),
            rng.uniform(5.0, 11.0),    # radius
        ))

    for y in range(height):
        base = 35 + (y * 35 // height)  # brushed steel vertical gradient
        for x in range(width):
            streak = ((x * 31 + y * 7) & 15) - 8

            # ── Height displacement (fakes surface micro-geometry) ──
            h_disp = 0.0

            # Grain noise
            nx = int(x * nw / width)
            ny = int(y * nh / height)
            h_disp += (noise[(ny % nh) * nw + (nx % nw)] - 128) * 0.35

            # Scratches
            for sx, sy, sa, sl, sw in scratches:
                d_local_x = x - sx
                d_local_y = y - sy
                along = d_local_x * math.cos(sa) + d_local_y * math.sin(sa)
                across = abs(-d_local_x * math.sin(sa) + d_local_y * math.cos(sa))
                if 0 <= along <= sl and across < sw:
                    f = (1.0 - across / sw) ** 2
                    ef = 1.0 - abs(along - sl/2) / (sl/2)
                    h_disp -= 60.0 * f * ef

            # Pits
            for px, py, pr, pd in pits:
                d2 = (x - px)**2 + (y - py)**2
                if d2 < pr*pr:
                    dist = math.sqrt(d2) / pr
                    h_disp -= 70.0 * pd * (1.0 - dist) ** 2

            # Rivets
            for rx, ry, rr in rivets:
                d2 = (x - rx)**2 + (y - ry)**2
                if d2 < rr*rr*1.5*1.5:
                    dist = math.sqrt(d2) / rr
                    if dist < 1.0:
                        h_disp += 55.0 * (1.0 - dist*dist)
                    elif dist < 1.5:
                        fade = (1.0 - (dist - 1.0) / 0.5) ** 2
                        h_disp -= 16.0 * fade

            # Edge shadow
            dx = (x - cx) / cx
            dy = (y - cy) / cy
            edge = dx*dx + dy*dy
            if edge > 0.7:
                h_disp -= 25.0 * ((edge - 0.7) / 0.3) ** 2

            # ── Perturb surface normal with height gradient ──
            # Sample height at neighbours for finite-difference normal
            h_c = h_disp
            def h_at(ox, oy):
                # Re-sampling h_disp at offsets is expensive but correct.
                # For simplicity, approximate the perturbation on the sphere
                # normal by directly modulating N·H based on a fake tangent
                # slope derived from the 2D height gradient in texture space.
                return h_c  # fallback: no perturbation for off-samples below
            # Approximate normal perturbation:
            #   perturbed N ≈ normalize(N + (dh/dx * T_x + dh/dy * T_y))
            # where T_x, T_y are tangent vectors in screen space.
            # Simpler: just modulate the specular contribution by height.
            # Positive h_disp = raised = catches more light = brighter.
            # Negative h_disp = recessed = darker.

            # ── Blinn-Phong specular on sphere ──
            dx_s = (x - cx) / cx
            dy_s = (y - cy) / cy
            d2_s = dx_s*dx_s + dy_s*dy_s
            if d2_s < 1.0:
                nz = math.sqrt(1.0 - d2_s)
                ndot_h = dx_s * hx + dy_s * hy + nz * hz
                if ndot_h < 0.0: ndot_h = 0.0
                spec = ndot_h ** shininess
                fresnel = 0.6 + 0.4 * (1.0 - nz)
            else:
                spec = 0.0
                fresnel = 1.0

            # Height modulates specular: raised = brighter, recessed = darker
            height_mod = 1.0 + h_disp / 255.0  # ~[0.7, 1.3]
            if height_mod < 0.5: height_mod = 0.5
            if height_mod > 1.5: height_mod = 1.5

            r = max(0, min(255, base + streak + int(spec * 180.0 * fresnel * height_mod)))
            g = max(0, min(255, base + streak + int(spec * 180.0 * fresnel * height_mod)))
            b = max(0, min(255, base + streak + int(spec * 200.0 * fresnel * height_mod) + 6))

            i = (y * width + x) * 3
            buf[i] = r; buf[i+1] = g; buf[i+2] = b
    return buf


def gen_specular(width: int, height: int) -> bytearray:
    """Specular highlight mask: Blinn-Phong reflection lobe on black.

    Maps a spherical surface normal across the texture UV space, computing
    pow(N·H, shininess) where H is the half-angle for a viewer at (0,0,1)
    and light above-left. Result is a concentrated highlight ring for the
    ADD multi-texture pass."""
    import math
    buf = bytearray(width * height * 3)
    cx, cy = width / 2.0, height / 2.0

    # Light above-left, viewer at normal
    lx, ly, lz = -0.45, -0.55, 0.70
    ll = math.sqrt(lx*lx + ly*ly + lz*lz)
    lx /= ll; ly /= ll; lz /= ll
    hx, hy, hz = lx, ly, lz + 1.0
    hl = math.sqrt(hx*hx + hy*hy + hz*hz)
    hx /= hl; hy /= hl; hz /= hl

    shininess = 128.0   # sharper than metal base for a tight highlight

    for y in range(height):
        for x in range(width):
            dx = (x - cx) / cx
            dy = (y - cy) / cy
            d2 = dx*dx + dy*dy
            if d2 < 1.0:
                nz = math.sqrt(1.0 - d2)
                ndot_h = dx * hx + dy * hy + nz * hz
                if ndot_h < 0.0:
                    ndot_h = 0.0
                v = int((ndot_h ** shininess) * 255.0)
            else:
                v = 0
            i = (y * width + x) * 3
            buf[i] = v; buf[i+1] = v; buf[i+2] = v
    return buf


def gen_reflect(width: int, height: int) -> bytearray:
    """Environment reflection map: spherical projection of sky + desert ground.

    UV mapped as a sphere-map: u = (R.x+1)/2, v = (R.z+1)/2 where R is the
    reflected view vector. For a cube face-normal viewer, this gives sky
    reflection near the top and ground near the bottom, with a smooth horizon.

    The texture is radially symmetric around the center, matching the
    Blinn-Phong specular lobe so that the combined effect (base metal +
    reflect ADD + specular ADD) reads as polished metal."""
    import math
    buf = bytearray(width * height * 3)
    cx, cy = width / 2.0, height / 2.0
    max_r = min(cx, cy)

    # Sky colors
    sky_top_r, sky_top_g, sky_top_b = 60, 100, 180     # deep blue
    sky_hor_r, sky_hor_g, sky_hor_b = 150, 190, 230    # horizon blue-white
    ground_r, ground_g, ground_b = 210, 170, 110        # desert sand

    for y in range(height):
        for x in range(width):
            dx = (x - cx) / max_r
            dy = (y - cy) / max_r

            # Sphere-map: reflected view vector R at pixel (dx, dy)
            # For a sphere-map: r² = dx² + dy², and R.z = 1 - r²
            r2 = dx*dx + dy*dy
            if r2 < 1.0:
                # R = (dx, dy, 1 - r2) normalized
                rz = 1.0 - r2  # actually this IS rz unnormalized since nz=1
                # Compute normalized R from the sphere-map projection
                inv_len = 1.0 / math.sqrt(r2 + rz*rz)
                dx_n = dx * inv_len
                dy_n = dy * inv_len
                rz_n = rz * inv_len

                # Use R.y (vertical component) for sky/ground blending
                ry = dy_n  # [-1, 1]  (-1 = ground, +1 = sky)

                if ry > 0.0:
                    # Sky side — blend deep blue → horizon white
                    t = ry   # 0 (horizon) → 1 (zenith)
                    r = int(sky_hor_r + (sky_top_r - sky_hor_r) * t)
                    g = int(sky_hor_g + (sky_top_g - sky_hor_g) * t)
                    b = int(sky_hor_b + (sky_top_b - sky_hor_b) * t)
                else:
                    # Ground side — sand
                    t = -ry   # 0 (horizon) → 1 (nadir)
                    r = int(ground_r - 30 * t)
                    g = int(ground_g - 40 * t)
                    b = int(ground_b - 35 * t)

                # Fade near the mirror edge (horizon blur)
                edge = 1.0 - r2   # 0 at edge, 1 at center
                if edge < 0.15:
                    f = edge / 0.15
                    r = int(f * r + (1 - f) * sky_hor_r)
                    g = int(f * g + (1 - f) * sky_hor_g)
                    b = int(f * b + (1 - f) * sky_hor_b)
            else:
                # Outside the sphere — horizon blue
                r, g, b = sky_hor_r, sky_hor_g, sky_hor_b

            i = (y * width + x) * 3
            buf[i] = max(0, min(255, r))
            buf[i+1] = max(0, min(255, g))
            buf[i+2] = max(0, min(255, b))
    return buf


def gen_horizon(width: int, height: int) -> bytearray:
    """Desert horizon: sand below, blue sky with clouds above, soft transition.

    Used for the four skybox side faces. Row 0 maps to the face bottom (v=0),
    so the sand occupies the low rows and the sky the high rows."""
    buf = bytearray(width * height * 3)
    horizon = 0.45
    band = 0.06
    clouds = [
        (0.25, 0.74, 0.16, 0.20),
        (0.62, 0.60, 0.20, 0.14),
        (0.48, 0.86, 0.18, 0.12),
        (0.85, 0.70, 0.13, 0.16),
        (0.12, 0.62, 0.15, 0.13),
        (0.55, 0.52, 0.22, 0.10),
    ]
    for y in range(height):
        v = y / height
        for x in range(width):
            u = x / width
            if v < horizon - band:
                n = ((x * 13 + y * 29) & 15) - 8
                r, g, b = 214 + n, 182 + n, 132 + n
            else:
                t = (v - horizon) / (1.0 - horizon)
                if t < 0.0:
                    t = 0.0
                sr = 70 + 60 * t
                sg = 120 + 60 * t
                sb = 190 + 40 * t
                cloud = 0.0
                for (cx, cy, rx, ry) in clouds:
                    dx = (u - cx) / rx
                    dy = (v - cy) / ry
                    d2 = dx * dx + dy * dy
                    if d2 < 1.0:
                        cloud += (1.0 - d2) * (1.0 - d2)
                if cloud > 1.0:
                    cloud = 1.0
                sr += (255 - sr) * cloud
                sg += (255 - sg) * cloud
                sb += (255 - sb) * cloud
                if v < horizon + band:
                    f = (v - (horizon - band)) / (2 * band)
                    n = ((x * 13 + y * 29) & 15) - 8
                    r = (214 + n) * (1 - f) + sr * f
                    g = (182 + n) * (1 - f) + sg * f
                    b = (132 + n) * (1 - f) + sb * f
                else:
                    r, g, b = sr, sg, sb
            i = (y * width + x) * 3
            buf[i] = max(0, min(255, int(r)))
            buf[i + 1] = max(0, min(255, int(g)))
            buf[i + 2] = max(0, min(255, int(b)))
    return buf


KINDS = {
    "ceramic":  gen_ceramic,
    "checker":  gen_checker,
    "brick":    gen_brick,
    "grid":     gen_grid,
    "sky":      gen_sky,
    "sand":     gen_sand,
    "horizon":  gen_horizon,
    "metal":    gen_metal,
    "specular": gen_specular,
    "reflect":  gen_reflect,
}


def emit_header(buf: bytes, name: str, width: int, height: int, path: str) -> None:
    with open(path, "w") as f:
        f.write("/* auto-generated by tools/gen_texture.py — do not edit */\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define {name.upper()}_WIDTH  {width}\n")
        f.write(f"#define {name.upper()}_HEIGHT {height}\n")
        f.write(f"static const uint8_t {name}_data[{len(buf)}] = {{\n")
        for i, b in enumerate(buf):
            if i % 16 == 0:
                f.write("    ")
            f.write(f"0x{b:02x},")
            if i % 16 == 15 or i == len(buf) - 1:
                f.write("\n")
            else:
                f.write(" ")
        f.write(f"}};\n")


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 1
    kind = argv[1]
    out_path = argv[2]
    name = argv[3] if len(argv) > 3 else f"texture_{kind}"
    width = int(argv[4]) if len(argv) > 4 else 256
    height = int(argv[5]) if len(argv) > 5 else 256
    if kind not in KINDS:
        print(f"unknown kind: {kind} (choose from {list(KINDS)})")
        return 1
    buf = KINDS[kind](width, height)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    emit_header(buf, name, width, height, out_path)
    print(f"gen_texture: wrote {out_path} ({len(buf)} bytes, "
          f"{width}x{height}, kind={kind}, name={name})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
