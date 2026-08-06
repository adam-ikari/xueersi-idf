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
        # Match horizon texture top edge (v=1): (130, 180, 230) at t=0,
        # zenith deep blue (70, 120, 190) at t=1.
        r = 130 - 60 * t
        g = 180 - 60 * t
        b = 230 - 40 * t
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
    """Brushed-metal (拉丝) base texture: regular directional streaks, low
    roughness.

    Replaces the earlier sand-cast heightfield (grain/pits/rivets/random
    scratches) which read as rough and dark. Brushed metal is characterised
    by fine, near-parallel surface striations laid down by an abrasive pad —
    uniform direction, gentle anisotropic shading, and a SMOOTH bright base.

    Why bright + low-contrast: TinyGL's RGB_MIX_FUNC is modulate
    (vertexColor × texel / 256, zbuffer.h:123-131). Specular adds to the
    vertex colour (light.c:391) BEFORE the texture modulates it, so a dark
    texel caps the highlight. The reflection overlay (unit1 ADD) is a
    separate true-ADD path and is NOT suppressed, but its perceived strength
    scales with how bright the whole surface reads. A bright, low-roughness
    base therefore lets both the Blinn-Phong specular and the sphere-map
    reflection read as polished metal instead of being crushed by a dark
    pitted texel. This is the "降低金属的粗糙度" lever — shininess alone
    (already 20 in wasm_game.cpp) was not enough; the texture itself was the
    roughness source.
    """
    import math, random as _random
    rng = _random.Random(42)  # deterministic

    buf = bytearray(width * height * 3)

    # Brush direction: horizontal streaks (拉丝 along +X). This is the
    # canonical look and, being axis-aligned, tiles seamlessly in X.
    # Streaks are built from a handful of long, faint, low-frequency
    # sinusoids along X plus a thin high-frequency component — regular, not
    # noisy. A small per-row phase drift gives the subtle waviness real
    # brushed metal shows without breaking the dominant direction.

    # Base polished-steel colour — bright so modulate doesn't crush the
    # highlight. ~190 on an 0..255 scale (≈0.75 reflectance).
    base = 190

    # Streak generators: (frequency in cycles/texel-x, amplitude, phase).
    # Low amp keeps the surface low-roughness (streaks vary ±~22 around base,
    # i.e. 168..212) so no dark valleys suppress specular/reflection.
    streaks = [
        (0.021, 11.0, 0.0),
        (0.037, 7.0, 1.3),
        (0.083, 4.5, 2.1),
        (0.17,  2.5, 0.6),
    ]

    # A faint cross-direction (vertical) large-scale modulation so the metal
    # isn't a perfectly uniform slab — reads as a slightly varying sheet.
    # Very low amplitude; this is NOT a second brush direction.
    cross_freq_y = 0.012
    cross_amp_y  = 6.0

    for y in range(height):
        # Per-row phase drift (slow) + deterministic jitter (tiny) so streaks
        # aren't a perfectly periodic comb — real brushed lines wander a hair.
        row_phase = 0.04 * y
        # Subtle vertical bright/dark banding from the cross modulation.
        vy = math.sin(2.0 * math.pi * cross_freq_y * y + 0.5)
        for x in range(width):
            s = 0.0
            for freq, amp, ph in streaks:
                s += amp * math.sin(2.0 * math.pi * freq * x + ph + row_phase)
            v = base + s + cross_amp_y * vy

            # A whisper of fine high-frequency grit along the brush lines so
            # the streaks have texture up close but stay low-contrast (±4).
            v += (rng.randint(0, 8) - 4)

            # Mild cool tint (steel): B a touch higher than R/G. Keep small
            # so the metal reads as neutral polished steel, not blue.
            r = v
            g = v
            b = v + 5

            r = max(0, min(255, int(r)))
            g = max(0, min(255, int(g)))
            b = max(0, min(255, int(b)))
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


def _reflect_color(ry: float,
                   sky_top, sky_hor, ground) -> tuple:
    """Map a reflection vector's Y component ry ∈ [-1,1] (+1=zenith/sky,
    -1=nadir/ground, 0=horizon) to an RGB triple. ry>0 blends horizon→sky,
    ry<0 blends horizon→ground. Shared by the in-disc and out-of-disc paths
    so the whole texture is coloured by R.y — never a flat horizon fill."""
    sky_top_r, sky_top_g, sky_top_b = sky_top
    sky_hor_r, sky_hor_g, sky_hor_b = sky_hor
    ground_r, ground_g, ground_b = ground
    if ry > 0.0:
        blend = ry
        return (int(sky_hor_r + (sky_top_r - sky_hor_r) * blend),
                int(sky_hor_g + (sky_top_g - sky_hor_g) * blend),
                int(sky_hor_b + (sky_top_b - sky_hor_b) * blend))
    else:
        blend = -ry
        return (int(sky_hor_r + (ground_r - sky_hor_r) * blend),
                int(sky_hor_g + (ground_g - sky_hor_g) * blend),
                int(sky_hor_b + (ground_b - sky_hor_b) * blend))


def gen_reflect(width: int, height: int) -> bytearray:
    """Environment reflection map for the cheap normal-driven sphere-map.

    The kernel (vertex.c) no longer uses the real sphere-projection math.
    It uses a cheap offset-scroll approximation driven by the eye-space
    normal:  s2 = 0.5 + 0.5·n.X,  t2 = 0.5 + 0.5·n.Y  (clamped [0,1]).
    So the texture's (s,t) layout MUST match that mapping, not the old
    valid-disc sphere-map layout:

      t axis (vertical) is the PRIMARY axis — it tracks n.Y:
        t=1 (top row,    n.Y=+1, normal pointing up)    -> SKY (zenith)
        t=0 (bottom row, n.Y=-1, normal pointing down)  -> GROUND (nadir)
        t=0.5 (middle,   n.Y= 0)                        -> HORIZON
      s axis (horizontal) tracks n.X — gives visible sideways scroll as
      the cube yaws. A gentle horizontal hue gradient (sky bluer on one
      side, warmer on the other; ground sandier vs rockier) makes the
      Y-rotation reflection walk readable, without breaking the sky/ground
      split that t alone determines.

    This is the "fake" reflection the user asked for: an OpenGL 1.x
    sphere-map API surface (glTexGeni GL_SPHERE_MAP + GL_TEXTURE_GEN_S/T)
    with the heavy math replaced by a 2-mul normal-driven offset, and a
    texture painted to match. Zero sqrt, zero reflection vector."""
    buf = bytearray(width * height * 3)

    # Sky / ground palette — high contrast for a mirror-like read.
    sky_top = (70, 130, 215)     # zenith deep blue
    sky_hor = (185, 210, 235)    # horizon blue-white
    ground  = (150, 115, 55)     # desert sand (nadir)
    ground_hor = (170, 140, 80)  # distant ground near horizon (warmer)

    for y in range(height):
        for x in range(width):
            # Texel -> (s,t). Image y=0 is the TOP row -> t=1 (sky).
            s = (x + 0.5) / width
            t = 1.0 - (y + 0.5) / height
            # ry in [-1,1]: +1=sky, -1=ground. This IS n.Y under the cheap map.
            ry = 2.0 * t - 1.0
            # Horizontal variation factor in [-1,1]: adds a sideways hue
            # shift so yaw rotation reads as a walking reflection. Small
            # amplitude — the sky/ground split stays governed by ry.
            hx = 2.0 * s - 1.0

            if ry >= 0.0:
                # Sky half: horizon (ry=0) -> zenith (ry=1). Shift the blue
                # toward a warmer/orangier horizon on one side (sun glow) so
                # horizontal motion has visible colour change.
                blend = ry
                r = sky_hor[0] + (sky_top[0] - sky_hor[0]) * blend
                g = sky_hor[1] + (sky_top[1] - sky_hor[1]) * blend
                b = sky_hor[2] + (sky_top[2] - sky_hor[2]) * blend
                # Sun-glow warmth on the +s side, cooler on -s side.
                glow = 0.18 * (1.0 - ry) * max(0.0, hx)
                r += 35.0 * glow
                g += 18.0 * glow
                b -= 10.0 * glow
            else:
                # Ground half: horizon (ry=0) -> nadir (ry=-1). Sandier near
                # horizon, rockier/darker at nadir; a slight sideways shift
                # so yaw motion reads.
                blend = -ry
                r = ground_hor[0] + (ground[0] - ground_hor[0]) * blend
                g = ground_hor[1] + (ground[1] - ground_hor[1]) * blend
                b = ground_hor[2] + (ground[2] - ground_hor[2]) * blend
                r += 10.0 * hx
                g += 6.0 * hx
                b += 2.0 * hx

            i = (y * width + x) * 3
            buf[i]   = max(0, min(255, int(r)))
            buf[i+1] = max(0, min(255, int(g)))
            buf[i+2] = max(0, min(255, int(b)))
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
                # Sand — smooth, no noise (was: n = ((x*13+y*29)&15)-8)
                r, g, b = 214, 182, 132
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
                    r = 214 * (1 - f) + sr * f
                    g = 182 * (1 - f) + sg * f
                    b = 132 * (1 - f) + sb * f
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


def write_bmp(buf: bytes, width: int, height: int, path: str) -> None:
    """Write a 24-bit BMP (bottom-up rows, BGR) so generated textures can be
    visually inspected. The .h header is what the firmware uses; the .bmp is a
    human-facing preview kept next to it in the build directory."""
    row_bytes = width * 3
    pad = (4 - row_bytes % 4) % 4
    padded_row = row_bytes + pad
    image_size = padded_row * height
    file_size = 14 + 40 + image_size
    with open(path, "wb") as f:
        # BITMAPFILEHEADER (14 bytes)
        f.write(b"BM")
        f.write(file_size.to_bytes(4, "little"))
        f.write((0).to_bytes(2, "little"))
        f.write((0).to_bytes(2, "little"))
        f.write((54).to_bytes(4, "little"))
        # BITMAPINFOHEADER (40 bytes)
        f.write((40).to_bytes(4, "little"))
        f.write(width.to_bytes(4, "little"))
        f.write(height.to_bytes(4, "little"))
        f.write((1).to_bytes(2, "little"))   # planes
        f.write((24).to_bytes(2, "little"))  # bpp
        f.write((0).to_bytes(4, "little"))   # compression
        f.write(image_size.to_bytes(4, "little"))
        f.write((2835).to_bytes(4, "little"))  # 72 DPI x
        f.write((2835).to_bytes(4, "little"))  # 72 DPI y
        f.write((0).to_bytes(4, "little"))
        f.write((0).to_bytes(4, "little"))
        # Pixel data: BMP rows are bottom-up, BGR. buf is top-down RGB.
        pad_bytes = b"\x00" * pad
        for y in range(height - 1, -1, -1):
            row = bytearray()
            base = y * width * 3
            for x in range(width):
                i = base + x * 3
                row.append(buf[i + 2])  # B
                row.append(buf[i + 1])  # G
                row.append(buf[i + 0])  # R
            f.write(row)
            f.write(pad_bytes)


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
    # Also write a .bmp preview (same stem) so generated textures can be
    # visually inspected without flashing the device.
    bmp_path = os.path.splitext(out_path)[0] + ".bmp"
    write_bmp(buf, width, height, bmp_path)
    print(f"gen_texture: wrote {out_path} ({len(buf)} bytes, "
          f"{width}x{height}, kind={kind}, name={name}) + {bmp_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
