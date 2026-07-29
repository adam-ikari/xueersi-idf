# TinyGL — vendored fork of C-Chads/tinygl @ 36a7987
#
# ESP-IDF component wrapping the software OpenGL rasterizer for the
# Xiaomiao ST7735 (160x128, RGB565, MADCTL_RGB → big-endian on the wire).
#
# Local patches on top of upstream (see commit history):
#   - 16-bit render path (TGL_FEATURE_16_BITS=1 / 32_BITS=0) → RGB565 PIXEL
#   - TGL_OPTIMIZATION_HINT_BRANCH_COST=0 (texture-coord interp must not be
#     branch-elided on the ESP32; upstream default 2 drops texcoords)
#   - TGL_PIXEL_BYTE_SWAP / TGL_TEXTURE_BYTE_SWAP macros: emit big-endian
#     RGB565 words so a little-endian CPU lays them out big-endian in the
#     framebuffer that esp_lcd_panel_io_tx_color ships as-is (no swap flag).
#
# Upstream: https://github.com/C-Chads/tinygl  (this is a maintained local copy;
# pulling upstream changes is a manual merge, not a submodule update).

idf_component_register(
    SRCS
        src/accum.c
        src/api.c
        src/arrays.c
        src/clear.c
        src/clip.c
        src/get.c
        src/image_util.c
        src/init.c
        src/light.c
        src/list.c
        src/matrix.c
        src/memory.c
        src/misc.c
        src/msghandling.c
        src/select.c
        src/specbuf.c
        src/texture.c
        src/vertex.c
        src/zbuffer.c
        src/zline.c
        src/zmath.c
        src/zpostprocess.c
        src/zraster.c
        src/ztext.c
        src/ztriangle.c
    INCLUDE_DIRS
        include
        src
)

# ST7735 expects big-endian RGB565 on the SPI wire (MADCTL_RGB, no I/O swap).
# TinyGL renders straight into the display backend's framebuffer, so its
# 16-bit PIXEL words must be byte-swapped at source.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TGL_PIXEL_BYTE_SWAP=1
    TGL_TEXTURE_BYTE_SWAP=1
)

# Software rasterizer on a single Xtens core; -O2 is the sweet spot and
# avoids the upstream -march=native (invalid on ESP32).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
