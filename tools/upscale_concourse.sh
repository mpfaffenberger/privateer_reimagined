#!/usr/bin/env bash
# AI-upscale every extracted concourse PNG 2x with Real-ESRGAN (anime model).
#
# Run AFTER tools/extract_wcu_concourse.py (which writes the low-res PNGs).
# The atlases' UVs are stored as ratios in concourse.json, so a uniform 2x
# needs NO manifest changes.
#
# Needs the standalone realesrgan-ncnn-vulkan binary + its models/ dir:
#   https://github.com/xinntao/Real-ESRGAN/releases  (macos zip)
# Point REALESRGAN_DIR at the unzipped folder (default /tmp/resrgan).
set -euo pipefail

DIR="${REALESRGAN_DIR:-/tmp/resrgan}"
BIN="$DIR/realesrgan-ncnn-vulkan"
MODELS="$DIR/models"
MODEL="${REALESRGAN_MODEL:-realesr-animevideov3-x2}"
SCALE="${REALESRGAN_SCALE:-2}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
[ -x "$BIN" ] || { echo "missing $BIN — download the Real-ESRGAN macos release"; exit 1; }
xattr -dr com.apple.quarantine "$DIR" 2>/dev/null || true

n=0
for f in "$REPO"/assets/concourse/*/*.png; do
    if "$BIN" -i "$f" -o /tmp/_upscale.png -m "$MODELS" -n "$MODEL" -s "$SCALE" >/dev/null 2>&1; then
        mv /tmp/_upscale.png "$f"; n=$((n+1)); echo "  up $(basename "$f")"
    else
        echo "  !! failed $(basename "$f")"
    fi
done
echo "upscaled $n concourse image(s) with $MODEL"
