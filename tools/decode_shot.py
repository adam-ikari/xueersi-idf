#!/usr/bin/env python3
"""Capture an ESP32 framebuffer 'shot' over serial and decode to a PNG.

Sends the `shot` command, reads between the ===SHOT BEGIN/END=== markers,
base64-decodes the RGB565-LE bytes, and writes a 160x128 PNG.

Usage: decode_shot.py /dev/cu.usbmodem1101 out.png
"""
import serial, time, sys, base64, struct

port = sys.argv[1]
out  = sys.argv[2] if len(sys.argv) > 2 else "shot.png"

s = serial.Serial(port, 115200, timeout=1.0, dsrdtr=False)
# open triggers an auto-reset; wait for boot
time.sleep(4.0)
s.read(65536)
# fresh prompt
s.write(b"\n"); s.flush(); time.sleep(0.3); s.read(65536)

# send shot
s.write(b"shot\n"); s.flush()

buf = b""
t0 = time.time()
collecting = False
hdr = None
while time.time() - t0 < 25:
    d = s.read(8192)
    if not d:
        if collecting and time.time() - t0 > 10:
            # try to end
            pass
        continue
    buf += d
    text = buf.decode("utf-8", "replace")
    if not collecting and "===SHOT BEGIN" in text:
        # parse header
        line = text[text.index("===SHOT BEGIN"):].split("\n", 1)[0]
        parts = line.split()
        # ===SHOT BEGIN 160 128 RGB565-LE base64===
        hdr = (int(parts[2]), int(parts[3]))  # w h
        body_start = text.index("===SHOT BEGIN") + len("===SHOT BEGIN")
        # find end of that header line
        nl = text.index("\n", body_start) + 1
        buf = buf[nl:]
        collecting = True
        t0 = time.time()  # reset timer for body
        continue
    if collecting and "===SHOT END===" in buf.decode("utf-8", "replace"):
        txt = buf.decode("utf-8", "replace")
        body = txt[: txt.index("===SHOT END===")]
        break
else:
    print("TIMEOUT collecting. got", len(buf), "bytes")
    sys.exit(1)

# body is base64 across multiple lines
b64 = "".join(body.split())
raw = base64.b64decode(b64)
w, h = hdr or (160, 128)
print(f"captured {len(raw)} bytes, {w}x{h}")

# raw is RGB565 little-endian per pixel. Convert to RGB888 -> PNG.
try:
    from PIL import Image
except ImportError:
    print("PIL not available; writing raw .rgb565 instead")
    open(out, "wb").write(raw)
    sys.exit(0)

pixels = []
for i in range(0, len(raw), 2):
    lo, hi = raw[i], raw[i+1]
    # raw is byte-swapped by TinyGL (TGL_PIXEL_BYTE_SWAP=1); un-swap to logical RGB565
    wv = (lo << 8) | hi
    r = (wv >> 11) & 0x1f
    g = (wv >> 5) & 0x3f
    b = wv & 0x1f
    pixels.append((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
img = Image.new("RGB", (w, h))
img.putdata(pixels)
img.save(out)
print(f"wrote {out}")
