#!/bin/bash
# Batch render multiple test scenes for offline comparison

set -e

EMU="./tinygl_emu"
OUT="bmp_scenes"

mkdir -p "$OUT"

echo "=== Rendering test scenes ==="

# Scene 1: 1 cube, multiple angles
mkdir -p "$OUT/cube1"
$EMU --render-bmp "$OUT/cube1" 8 1

# Scene 2: 4 cubes
mkdir -p "$OUT/cube4"
$EMU --render-bmp "$OUT/cube4" 4 4

# Scene 3: 9 cubes
mkdir -p "$OUT/cube9"
$EMU --render-bmp "$OUT/cube9" 2 9

# Scene 4: 16 cubes
mkdir -p "$OUT/cube16"
$EMU --render-bmp "$OUT/cube16" 1 16

echo "=== Done ==="
echo "Output: $OUT/"
ls -la "$OUT"/*/
