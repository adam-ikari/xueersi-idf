#!/usr/bin/env python3
"""
xiaomiao-debug MCP Server — serial debug console bridge for ESP32 TinyGL.

Exposes the debug REPL commands as MCP tools so Claude Desktop can probe
the live framebuffer, texture pixmap, and GL state without writing one-off
scripts every time.

Start: python tools/mcp-server/server.py --port /dev/cu.usbmodem1101
"""

import json, sys, base64, time, struct
from typing import Any

import serial
from mcp.server import Server, NotificationOptions
from mcp.server.models import InitializationCapabilities
from mcp.server.stdio import stdio_server

PORT = "/dev/cu.usbmodem1101"
BAUD = 115200

# ── serial helpers ────────────────────────────────────────────────────────

def open_serial():
    s = serial.Serial(PORT, BAUD, timeout=1.0, dsrdtr=False)
    time.sleep(4.0)  # wait for ESP32 boot
    s.read(65536)     # drain boot log
    return s

def cmd(s: serial.Serial, text: str, wait: float = 0.6) -> str:
    """Send a command to the REPL and return the response line(s)."""
    s.write((text + "\n").encode())
    s.flush()
    time.sleep(wait)
    return s.read(65536).decode("utf-8", "replace")

# ── shared serial connection (lazy init) ──────────────────────────────────

_serial: serial.Serial | None = None

def get_serial():
    global _serial
    if _serial is None or not _serial.is_open:
        _serial = open_serial()
    return _serial

# ── low-level helpers ─────────────────────────────────────────────────────

def parse_fb_output(out: str) -> dict | None:
    """Parse 'fb[x,y] = 0xNNNN  -> R... G... B...' output."""
    import re
    m = re.search(r"fb\[(\d+),(\d+)\]\s*=\s*(0x[0-9a-fA-F]+)\s*->\s*R(\d+)\s*G(\d+)\s*B(\d+)", out)
    if m:
        return {
            "x": int(m.group(1)), "y": int(m.group(2)),
            "raw_565": int(m.group(3), 16),
            "r": int(m.group(4)), "g": int(m.group(5)), "b": int(m.group(6)),
        }
    return None

def parse_state_output(out: str) -> dict:
    """Parse the state command output into a dict."""
    import re
    result = {}
    patterns = {
        "zb_ptr": r"ZBuffer\s*:\s*(\S+)",
        "pbuf": r"pbuf\s*=\s*(\S+)",
        "zbuf": r"zbuf\s*=\s*(\S+)",
        "current_tex": r"current_tex\s*=\s*(\S+)",
        "xsize": r"xsize/ysize\s*=\s*(\d+)\s*x\s*(\d+)",
        "pixel_swap": r"TGL_PIXEL_BYTE_SWAP=(\d+)",
        "texture_swap": r"TGL_TEXTURE_BYTE_SWAP=(\d+)",
        "render_bits": r"RENDER_BITS=(\d+)",
        "texture_dim": r"TEXTURE_DIM=(\d+)",
        "cur_texture_ptr": r"current_texture=(\S+)",
        "tex_2d_enabled": r"texture_2d_enabled=(\d+)",
    }
    for key, pat in patterns.items():
        m = re.search(pat, out)
        if m:
            if key in ("xsize",):
                result[key] = (int(m.group(1)), int(m.group(2)))
            else:
                result[key] = m.group(1)
    return result

def parse_tex_output(out: str) -> dict | None:
    """Parse 'tex[idx] = 0xNNNN -> R... G... B...' output."""
    import re
    m = re.search(r"tex\[(\d+)\]\s*=\s*(0x[0-9a-fA-F]+)\s*->\s*R(\d+)\s*G(\d+)\s*B(\d+)", out)
    if m:
        return {
            "index": int(m.group(1)),
            "raw_565": int(m.group(2), 16),
            "r": int(m.group(3)), "g": int(m.group(4)), "b": int(m.group(5)),
        }
    return None

def parse_shot_output(out: str) -> list[list[tuple[int,int,int]]]:
    """Parse the hex shot output into a 2D array of (r,g,b) tuples."""
    import re
    # Find the header line
    m = re.search(r"===SHOT BEGIN (\d+) (\d+)===", out)
    if not m:
        return []
    w, h = int(m.group(1)), int(m.group(2))
    body_start = out.index("===\n") + 4 if "===\n" in out else out.index("===\r\n") + 5
    lines = out[body_start:].split("\n")
    pixels = []
    for line in lines:
        if "===" in line or not line.strip():
            continue
        row = []
        hex_line = line.strip()
        for i in range(0, len(hex_line), 4):
            chunk = hex_line[i:i+4]
            if len(chunk) != 4:
                continue
            try:
                raw = int(chunk, 16)
                # raw is stored (possibly swapped), decode via fb probe logic
                # For now store raw, decode happens client-side
                row.append(raw)
            except ValueError:
                continue
        if row:
            pixels.append(row)
    return pixels

# ── MCP tool implementations ──────────────────────────────────────────────

async def tool_fb_probe(x: int, y: int) -> str:
    """Probe a single framebuffer pixel at (x,y)."""
    s = get_serial()
    out = cmd(s, f"fb {x} {y}")
    parsed = parse_fb_output(out)
    if parsed:
        return json.dumps(parsed)
    return json.dumps({"error": "parse failed", "raw": out.strip()[:200]})

async def tool_fb_rect(x0: int, y0: int, x1: int, y1: int) -> str:
    """Probe a rectangular region of framebuffer pixels."""
    s = get_serial()
    cmd(s, "pause", 0.5)
    pixels = []
    for y in range(y0, y1 + 1):
        row = []
        for x in range(x0, x1 + 1):
            out = cmd(s, f"fb {x} {y}", 0.2)
            parsed = parse_fb_output(out)
            if parsed:
                row.append({"x": x, "y": y, "r": parsed["r"], "g": parsed["g"], "b": parsed["b"]})
        if row:
            pixels.append(row)
    return json.dumps({"width": x1 - x0 + 1, "height": y1 - y0 + 1, "pixels": pixels})

async def tool_fb_screenshot() -> str:
    """Take a base64-encoded screenshot of the entire framebuffer (PNG)."""
    s = get_serial()
    # Pause, then shot
    cmd(s, "pause", 0.5)
    s.write(b"shot\n")
    s.flush()
    time.sleep(15.0)
    buf = s.read(655360)
    out = buf.decode("utf-8", "replace")
    pixels = parse_shot_output(out)
    if not pixels:
        return json.dumps({"error": "shot parse failed", "raw_len": len(out)})
    # Convert to PNG using raw pixel values
    try:
        from PIL import Image
        h = len(pixels)
        w = len(pixels[0]) if h > 0 else 0
        img_data = []
        for row in pixels:
            for raw in row:
                # Un-swap (TinyGL stores byte-swapped via TGL_PIXEL_BYTE_SWAP=1)
                lo, hi = raw & 0xff, (raw >> 8) & 0xff
                wv = (lo << 8) | hi
                r = (wv >> 11) & 0x1f
                g = (wv >> 5) & 0x3f
                b = wv & 0x1f
                img_data.append((r << 3, g << 2, b << 3))
        img = Image.new("RGB", (w, h))
        img.putdata(img_data)
        import io
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        b64 = base64.b64encode(buf.getvalue()).decode()
        return json.dumps({"format": "png", "width": w, "height": h, "base64": b64})
    except ImportError:
        return json.dumps({"error": "PIL not available", "width": w if 'w' in dir() else 0, "height": h if 'h' in dir() else 0})

async def tool_tex_probe(idx: int) -> str:
    """Probe a single texture pixmap pixel at linear index idx."""
    s = get_serial()
    out = cmd(s, f"tex {idx}")
    parsed = parse_tex_output(out)
    if parsed:
        return json.dumps(parsed)
    return json.dumps({"error": "parse failed", "raw": out.strip()[:200]})

async def tool_state_dump() -> str:
    """Dump live ZBuffer, GL config, and render state."""
    s = get_serial()
    out = cmd(s, "state")
    parsed = parse_state_output(out)
    return json.dumps(parsed)

async def tool_render_control(action: str, r: int = 0, g: int = 0, b: int = 0) -> str:
    """Control the render loop: pause, resume, clear."""
    s = get_serial()
    if action == "pause":
        out = cmd(s, "pause")
    elif action == "resume":
        out = cmd(s, "resume")
    elif action == "clear":
        out = cmd(s, f"clear {r} {g} {b}")
    else:
        return json.dumps({"error": f"unknown action: {action}"})
    return json.dumps({"action": action, "response": out.strip()[:200]})

# ── MCP Server ────────────────────────────────────────────────────────────

async def main():
    server = Server("xiaomiao-debug")

    @server.list_tools()
    async def list_tools():
        return [
            {"name": "fb_probe",      "description": "Read a single framebuffer pixel at (x,y). Returns RGB565 decoded values.", "inputSchema": {"type": "object", "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}}, "required": ["x", "y"]}},
            {"name": "fb_rect",       "description": "Read a rectangle of framebuffer pixels from (x0,y0) to (x1,y1).", "inputSchema": {"type": "object", "properties": {"x0": {"type": "integer"}, "y0": {"type": "integer"}, "x1": {"type": "integer"}, "y1": {"type": "integer"}}, "required": ["x0", "y0", "x1", "y1"]}},
            {"name": "fb_screenshot", "description": "Capture the entire 160x128 framebuffer as a base64-encoded PNG.", "inputSchema": {"type": "object", "properties": {}}},
            {"name": "tex_probe",     "description": "Read a texture pixmap pixel at linear index.", "inputSchema": {"type": "object", "properties": {"idx": {"type": "integer"}}, "required": ["idx"]}},
            {"name": "state_dump",    "description": "Dump ZBuffer pointers, GL config, byte-swap flags.", "inputSchema": {"type": "object", "properties": {}}},
            {"name": "render_control", "description": "Control render loop: pause, resume, clear(r,g,b).", "inputSchema": {"type": "object", "properties": {"action": {"type": "string", "enum": ["pause", "resume", "clear"]}, "r": {"type": "integer"}, "g": {"type": "integer"}, "b": {"type": "integer"}}, "required": ["action"]}},
        ]

    @server.call_tool()
    async def call_tool(name: str, arguments: dict[str, Any]) -> list[Any]:
        try:
            if name == "fb_probe":
                return [{"type": "text", "text": await tool_fb_probe(arguments["x"], arguments["y"])}]
            elif name == "fb_rect":
                return [{"type": "text", "text": await tool_fb_rect(arguments["x0"], arguments["y0"], arguments["x1"], arguments["y1"])}]
            elif name == "fb_screenshot":
                return [{"type": "text", "text": await tool_fb_screenshot()}]
            elif name == "tex_probe":
                return [{"type": "text", "text": await tool_tex_probe(arguments["idx"])}]
            elif name == "state_dump":
                return [{"type": "text", "text": await tool_state_dump()}]
            elif name == "render_control":
                return [{"type": "text", "text": await tool_render_control(arguments["action"], arguments.get("r", 0), arguments.get("g", 0), arguments.get("b", 0))}]
            else:
                return [{"type": "text", "text": f"unknown tool: {name}"}]
        except Exception as e:
            return [{"type": "text", "text": json.dumps({"error": str(e)})}]

    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())

if __name__ == "__main__":
    import asyncio
    asyncio.run(main())
