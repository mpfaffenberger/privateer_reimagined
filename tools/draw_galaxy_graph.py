#!/usr/bin/env python3
"""Render assets/galaxy.json as a force-directed jump-graph image (PIL only)."""
import json, math, random
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[1]
gal = json.loads((REPO / "assets" / "galaxy.json").read_text())
nodes = [s["id"] for s in gal["systems"]]
disp  = {s["id"]: s["display_name"] for s in gal["systems"]}
sect  = {s["id"]: s.get("sector", "") for s in gal["systems"]}
idx   = {n: i for i, n in enumerate(nodes)}
N = len(nodes)

# undirected edges
E = set()
for j in gal["jumps"]:
    a, b = j["from"], j["to"]
    if a in idx and b in idx:
        E.add((min(a, b), max(a, b)))
edges = [(idx[a], idx[b]) for a, b in E]

# ---- Fruchterman-Reingold layout -------------------------------------------
random.seed(7)
W = H = 1.0
pos = [[random.random(), random.random()] for _ in range(N)]
k = math.sqrt(W * H / N) * 0.9
adj = [[] for _ in range(N)]
for a, b in edges:
    adj[a].append(b); adj[b].append(a)

for it in range(600):
    t = 0.10 * (1.0 - it / 600.0)          # cooling
    disp_v = [[0.0, 0.0] for _ in range(N)]
    for i in range(N):
        for jx in range(i + 1, N):
            dx = pos[i][0] - pos[jx][0]; dy = pos[i][1] - pos[jx][1]
            d2 = dx*dx + dy*dy + 1e-6; d = math.sqrt(d2)
            f = (k*k) / d2                  # repulsion
            ux, uy = dx/d, dy/d
            disp_v[i][0] += ux*f; disp_v[i][1] += uy*f
            disp_v[jx][0] -= ux*f; disp_v[jx][1] -= uy*f
    for a, b in edges:                      # attraction
        dx = pos[a][0]-pos[b][0]; dy = pos[a][1]-pos[b][1]
        d = math.sqrt(dx*dx+dy*dy)+1e-6
        f = d*d/k; ux, uy = dx/d, dy/d
        disp_v[a][0] -= ux*f; disp_v[a][1] -= uy*f
        disp_v[b][0] += ux*f; disp_v[b][1] += uy*f
    for i in range(N):
        dl = math.hypot(*disp_v[i]) + 1e-9
        pos[i][0] += disp_v[i][0]/dl * min(dl, t)
        pos[i][1] += disp_v[i][1]/dl * min(dl, t)

# normalize to canvas
xs = [p[0] for p in pos]; ys = [p[1] for p in pos]
minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
PX, PAD = 2400, 130
def sx(x): return PAD + (x-minx)/(maxx-minx+1e-9)*(PX-2*PAD)
def sy(y): return PAD + (y-miny)/(maxy-miny+1e-9)*(PX-2*PAD)

QCOL = {
    "Fariss Quadrant":   (90, 170, 255),
    "Clarke Quadrant":   (120, 230, 140),
    "Humboldt Quadrant": (255, 180, 90),
    "Potter Quadrant":   (235, 120, 235),
}
def col(i): return QCOL.get(sect[nodes[i]], (200, 200, 200))

img = Image.new("RGB", (PX, PX), (8, 10, 18))
d = ImageDraw.Draw(img)
try:
    fnt = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 18)
    big = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 40)
except Exception:
    fnt = ImageFont.load_default(); big = fnt

for a, b in edges:
    d.line([sx(pos[a][0]), sy(pos[a][1]), sx(pos[b][0]), sy(pos[b][1])],
           fill=(60, 70, 95), width=2)
for i in range(N):
    x, y = sx(pos[i][0]), sy(pos[i][1])
    r = 9 + min(8, len(adj[i]))
    c = col(i)
    d.ellipse([x-r, y-r, x+r, y+r], fill=c, outline=(15, 18, 28), width=2)
    d.text((x+r+3, y-10), disp[nodes[i]], fill=(225, 230, 240), font=fnt)

d.text((40, 36), f"Gemini Sector jump graph  \u2014  {N} systems, {len(edges)} links",
       fill=(235, 240, 250), font=big)
ly = 96
for name, c in QCOL.items():
    d.ellipse([44, ly+4, 64, ly+24], fill=c)
    d.text((74, ly), name, fill=(210, 216, 226), font=fnt); ly += 30

out = Path("/tmp/galaxy_graph.png")
img.save(out)
print("wrote", out)
