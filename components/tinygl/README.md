# TinyGL (vendored fork)

This is a **vendored, locally maintained** copy of [TinyGL](https://github.com/C-Chads/tinygl)
at upstream commit `36a7987` ("The ultimate portable graphics library", a
modernized fork of Fabrice Bellard's TinyGL).

It is **not** a git submodule. Upstream changes are merged in manually; the
source tree in `src/` and `include/` is tracked directly in this repo.

## Why vendored

The Xiaomiao ESP32-WROVER-B drives an ST7735 (160×128, RGB565, `MADCTL_RGB`).
That combination expects **big-endian RGB565 on the SPI wire**, and
`esp_lcd_panel_io_tx_color` is configured without the `swap_bytes` flag — so
the framebuffer bytes must already be big-endian. Upstream TinyGL emits
little-endian (native) RGB565, which produces the green→red gradient bug
documented in `TODO_TINYGL.md`. Rather than carry patch files against a
submodule, we maintain the patched source directly.

## Local patches vs upstream `36a7987`

| Change | File | Reason |
|---|---|---|
| `TGL_FEATURE_16_BITS=1` / `TGL_FEATURE_32_BITS=0` | `include/zfeatures.h` | 16-bit RGB565 render path (`PIXEL=GLushort`, `ZB_MODE_5R6G5B`) to match ST7735 |
| `TGL_OPTIMIZATION_HINT_BRANCH_COST=0` | `include/zfeatures.h` | Upstream default `2` elides texture-coordinate interpolation on ESP32; `0` keeps texcoords |
| `TGL_PIXEL_BYTE_SWAP` / `TGL_TEXTURE_BYTE_SWAP` macros + `TGL_BSWAP16` | `include/zfeatures.h` | Configurable big-endian pixel emit (off by default upstream) |
| `RGB_TO_PIXEL` swaps via `TGL_BSWAP16` (16-bit branch) | `include/zbuffer.h` | Framebuffer pixels ship big-endian to ST7735 |
| `gl_convertRGB_to_5R6G5B` swaps via `TGL_TEXTURE_BYTE_SWAP` | `src/image_util.c` | Texture pixmap matches framebuffer byte order |

`CMakeLists.txt` sets `TGL_PIXEL_BYTE_SWAP=1` / `TGL_TEXTURE_BYTE_SWAP=1` at
compile time to match the ST7735 backend.

## Component layout

```
components/tinygl/
├── CMakeLists.txt     # ESP-IDF component (SRCS glob of src/*.c)
├── LICENSE
├── include/           # GL/gl.h, zbuffer.h, zfeatures.h
└── src/               # tgl engine sources
```

Consumers (e.g. `main/`) depend on component name **`tinygl** and include
`"GL/gl.h"`, `"zbuffer.h"`, `"zfeatures.h"` (public include dir).
