---
name: xiaomiao-debug
description: Use when debugging the TinyGL 3D engine on the Xiaomiao ESP32 handheld — probe live framebuffer pixels, texture pixmap data, GL state, and control the render loop. Triggers on mentions of TinyGL, texture color, framebuffer, ESP32 debug, Xiaomiao screen.
---

# Xiaomiao TinyGL Debug

You have access to the `xiaomiao-debug` MCP server which talks to the ESP32 over serial UART. The device runs a REPL debug console that exposes TinyGL's internal state.

## Available MCP Tools

| Tool | Purpose |
|------|---------|
| `fb_probe(x, y)` | Read one framebuffer pixel. Returns `{r,g,b,raw_565}` |
| `fb_rect(x0,y0,x1,y1)` | Read a rectangle of pixels |
| `fb_screenshot` | Capture entire 160×128 framebuffer as base64 PNG |
| `tex_probe(idx)` | Read one texture pixmap pixel (256×256 linear index) |
| `state_dump` | Dump ZBuffer pointers, byte-swap flags, GL config |
| `render_control(action, r?, g?, b?)` | `pause`/`resume` render loop or `clear` screen |

## Key Facts

- Framebuffer: 160×128 RGB565, byte-swapped (ST7735 big-endian wire format)
- TinyGL renders directly to display buffer (`zb->pbuf` == display backend `s_fb`)
- `TGL_PIXEL_BYTE_SWAP=1` — framebuffer pixels are byte-swapped
- `TGL_TEXTURE_BYTE_SWAP=1` — texture pixmap pixels are byte-swapped
- Texture: 256×256 RGB565, uploaded from compile-time `texture_ceramic.h`
- Ceramic base color: R≈210 G≈180 B≈140 (warm beige)
- Clear color: `0xc618` = R3 G6 B6 (dark blue-grey)

## Workflow for Color Debugging

1. **Pause the render loop** so frames don't overwrite your probes:
   ```
   render_control("pause")
   ```

2. **Check state** to verify pointers and flags:
   ```
   state_dump → check pbuf address, byte-swap flags, texture_2d_enabled
   ```

3. **Probe texture pixmap** at known positions to verify upload:
   ```
   tex_probe(0)    → first pixel (dark rim area, R≈141 G≈120 B≈92)
   tex_probe(256*128+128) → center pixel
   ```

4. **Probe framebuffer** where the cube faces should be:
   ```
   fb_probe(80, 64) → center of screen
   fb_rect(60, 50, 100, 80) → cube face region
   ```

5. **Compare**: texture pixmap color vs framebuffer pixel color
   - If framebuffer ≈ texture pixmap at corresponding texcoords → color correct
   - If framebuffer is `0xc618` everywhere → rasterization not working
   - If colors are tinted → check `glColor3f` and `TGL_FEATURE_LIT_TEXTURES`

6. **Screenshot** for full visual confirmation:
   ```
   fb_screenshot → base64 PNG
   ```

## Interpreting Pixel Values

The MCP returns decoded RGB (0-255 range). The raw 565 value is also included. On SWAP builds:
- Raw pbuf value is byte-swapped (e.g., `0xcd8d` in pbuf = logical `0x8dcd`)
- The decode helper in the MCP un-swaps automatically

## Render Control

- `pause` — freeze the render loop (FPS drops to 0), framebuffer stays frozen
- `resume` — restart rendering
- `clear(r,g,b)` — fill entire framebuffer with a solid color + flush (pauses render)

## Safety

- Always `pause` before probing multiple pixels to avoid race conditions
- Always `resume` after debugging so the benchmark continues
- The serial port is `/dev/cu.usbmodem1101` at 115200 baud
- The MCP server handles serial connection lifecycle automatically