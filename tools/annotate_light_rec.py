#!/usr/bin/env python3
"""Visual check for sprite_light_rec (#111): runs the C++ matcher (/tmp/tlr, built from tools/light_rec_probe.cpp)
for a seed light and burns the result onto a copy of the sprite so we can
eyeball that recommendations land on the CENTRE of matching features.

    python3 tools/annotate_light_rec.py <png> <u> <v> [tol] [patch] [out.png]

Red ring = the query light you 'placed'. Green numbered dots = recommended
region centroids (exactly what the editor would accept)."""
import subprocess, sys, re
from PIL import Image, ImageDraw

png, u, v = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
tol   = sys.argv[4] if len(sys.argv) > 4 else "28"
patch = sys.argv[5] if len(sys.argv) > 5 else "6"
out   = sys.argv[6] if len(sys.argv) > 6 else "/tmp/light_rec_annotated.png"

res = subprocess.run(["/tmp/tlr", png, str(u), str(v), tol, patch],
                     capture_output=True, text=True)
print(res.stdout.strip())
cands = [(float(m[0]), float(m[1]), float(m[2])) for m in
         re.findall(r"\(([\d.]+),\s*([\d.]+)\)\s*score=([\d.]+)", res.stdout)]

img = Image.open(png).convert("RGBA")
W, H = img.size
# Composite over dark grey so transparent areas read like the in-game void.
bg = Image.new("RGBA", (W, H), (24, 24, 28, 255))
bg.alpha_composite(img)
d = ImageDraw.Draw(bg)

def ring(x, y, r, color, width=3):
    d.ellipse([x - r, y - r, x + r, y + r], outline=color, width=width)

# Query light (red).
qx, qy = u * W, v * H
ring(qx, qy, 9, (255, 40, 40, 255), 3)
d.ellipse([qx - 3, qy - 3, qx + 3, qy + 3], fill=(255, 40, 40, 255))

# Recommendations (green), numbered best-first.
for i, (cu, cv, sc) in enumerate(cands):
    cx, cy = cu * W, cv * H
    ring(cx, cy, 9, (60, 230, 90, 255), 3)
    d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=(60, 230, 90, 255))
    d.text((cx + 11, cy - 7), f"{i+1}", fill=(60, 230, 90, 255))

bg.convert("RGB").save(out)
print(f"wrote {out}  ({len(cands)} recommendations burned in)")
