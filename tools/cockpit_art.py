#!/usr/bin/env python3
"""Cockpit overlay art pipeline (#426 / #432).

Turns a generated cockpit painting into a game-ready RGBA overlay and
measures its display glass for src/cockpit_overlay_layout.h.

    python3 tools/cockpit_art.py finalize RAW.png OUT.png [--key RRGGBB] [--pad-top N]
    python3 tools/cockpit_art.py analyze  ART.png

finalize
    * Native-alpha input (the image generator can emit real transparency):
      alpha is kept, near-opaque snapped to 255 and specks to 0.
    * Keyed input (--key, RGB art on a flat colour): alpha comes from the
      colour distance to the key, with a soft 2-level ramp.
    * --pad-top N: mirror the top N rows above the canvas. Use it for art whose
      ceiling is solid metal but whose glass sits within ~24 px of the top:
      the pilot-head overscan would crop that glass at narrow windows. The
      mirrored band falls inside the overscan, and the 16:9 trim below takes
      the same N rows off the bottom.
    * Canvas trimmed from the bottom to >= 16:9 (see finalize()).
    * Edge decontamination: every partially transparent pixel takes the RGB
      of the nearest fully opaque pixel. That strips whatever matte the edge
      was blended against (white, magenta, green), which is what made the old
      pink rims. Colour then "bleeds" a few px into the clear area (alpha
      stays 0) so linear texture filtering never pulls a dark or coloured
      halo into the edge.

analyze
    Lists every enclosed transparent hole that is display glass (canopy panes
    are skipped: glass has straight edges and fills its quad), measures each as a
    perspective quad (least-squares edge lines through the alpha=128
    crossings, clear of the rounded corners, corners = line intersections),
    and prints ready-to-paste CockpitArt display rows in Display-enum order:
    Left, Center, Right (the three largest, by x), Banner (the topmost
    small one), SetSpeed and Velocity (the other small ones, by x). It also
    suggests a boresight row: open canopy between arch and dash.

PIL only (no numpy in the toolchain).
"""
import argparse
import math
from collections import deque

from PIL import Image

OPAQUE_SNAP = 240     # alpha >= this -> 255
SPECK_SNAP = 12       # alpha <= this -> 0
BLEED_PX = 4          # colour bleed into clear pixels (alpha stays 0)
MIN_HOLE_AREA = 400   # px; smaller enclosed holes are not displays


# ---- finalize ---------------------------------------------------------------

def key_alpha(img, key):
    """Alpha from colour distance to `key`: <60 clear, >140 solid, ramp between."""
    kr, kg, kb = key
    out = Image.new("RGBA", img.size)
    src, dst = img.convert("RGB").load(), out.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            d = math.sqrt((r - kr) ** 2 + (g - kg) ** 2 + (b - kb) ** 2)
            a = 0 if d < 60 else 255 if d > 140 else round((d - 60) * 255 / 80)
            dst[x, y] = (r, g, b, a)
    return out


def pad_top(img, n):
    """Grow the canvas upward by `n` rows mirrored from its top edge."""
    w, h = img.size
    out = Image.new("RGBA", (w, h + n))
    out.paste(img.crop((0, 0, w, n)).transpose(Image.Transpose.FLIP_TOP_BOTTOM), (0, 0))
    out.paste(img, (0, n))
    return out


def finalize(src_path, out_path, key=None, top=0):
    img = Image.open(src_path)
    img = key_alpha(img, key) if key else img.convert("RGBA")
    if top:
        img = pad_top(img, top)
    w, h = img.size
    # Generators emit near-16:9 canvases (1672x941 is 0.06% too tall). Trim
    # dash rows off the bottom until the art is >= 16:9, so a 16:9 window is
    # covered edge to edge by the height fit (the bottom is cropped by the
    # boresight slide anyway).
    if w * 9 < h * 16:
        h = w * 9 // 16 + (1 if (w * 9) % 16 else 0)
        h = min(h, img.size[1])
        while w * 9 < h * 16:
            h -= 1
        img = img.crop((0, 0, w, h))
    px = img.load()

    # Snap alpha, then multi-source BFS outward from fully opaque pixels so
    # every other pixel learns its nearest opaque colour.
    colour = {}
    q = deque()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            a = 255 if a >= OPAQUE_SNAP else 0 if a <= SPECK_SNAP else a
            px[x, y] = (r, g, b, a)
            if a == 255:
                colour[(x, y)] = (r, g, b, 0)
                q.append((x, y))
    reach = BLEED_PX + 8
    while q:
        x, y = q.popleft()
        r, g, b, d = colour[(x, y)]
        if d >= reach:
            continue
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in colour:
                colour[(nx, ny)] = (r, g, b, d + 1)
                q.append((nx, ny))

    fixed = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 255:
                continue
            c = colour.get((x, y))
            if a > 0 and c:
                px[x, y] = (c[0], c[1], c[2], a)          # decontaminated edge
                fixed += 1
            elif a == 0:
                px[x, y] = (c[0], c[1], c[2], 0) if c and c[3] <= BLEED_PX else (0, 0, 0, 0)
    img.save(out_path, optimize=True)
    print(f"{src_path} -> {out_path}  {w}x{h}, {fixed} edge px decontaminated")


# ---- analyze ----------------------------------------------------------------

def holes(alpha, w, h):
    """Enclosed transparent components (not touching the border)."""
    seen = bytearray(w * h)
    found = []
    for sy in range(h):
        for sx in range(w):
            if seen[sy * w + sx] or alpha[sx, sy] >= 128:
                continue
            q = deque([(sx, sy)])
            seen[sy * w + sx] = 1
            n, x0, x1, y0, y1, border = 0, sx, sx, sy, sy, False
            while q:
                x, y = q.popleft()
                n += 1
                x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
                border |= x in (0, w - 1) or y in (0, h - 1)
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and alpha[nx, ny] < 128:
                        seen[ny * w + nx] = 1
                        q.append((nx, ny))
            if not border and n >= MIN_HOLE_AREA:
                found.append({"area": n, "bbox": (x0, y0, x1, y1)})
    return found


def crossing(walk):
    prev = pc = None
    for c, v in walk:
        if prev is not None and (prev >= 128) != (v >= 128):
            return pc + (c - pc) * (128 - prev) / (v - prev)
        prev, pc = v, c
    return None


def fit(pts):
    n = len(pts)
    mv = sum(p[0] for p in pts) / n
    mu = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mv) ** 2 for p in pts)
    k = sum((p[0] - mv) * (p[1] - mu) for p in pts) / sxx if sxx else 0.0
    return k, mu - k * mv


def measure_quad(alpha, w, h, bbox):
    x0, y0, x1, y1 = bbox
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    mh, mw = max(2, int((y1 - y0) * 0.2)), max(2, int((x1 - x0) * 0.2))
    edges = {"L": [], "R": [], "T": [], "B": []}
    for y in range(y0 + mh, y1 - mh + 1):
        edges["L"].append((y + .5, crossing([(x + .5, alpha[x, y]) for x in range(cx, max(-1, x0 - 8), -1)])))
        edges["R"].append((y + .5, crossing([(x + .5, alpha[x, y]) for x in range(cx, min(w, x1 + 8))])))
    for x in range(x0 + mw, x1 - mw + 1):
        edges["T"].append((x + .5, crossing([(y + .5, alpha[x, y]) for y in range(cy, max(-1, y0 - 8), -1)])))
        edges["B"].append((x + .5, crossing([(y + .5, alpha[x, y]) for y in range(cy, min(h, y1 + 8))])))
    lines, resid = {}, 0.0
    for k, pts in edges.items():
        pts = [p for p in pts if p[1] is not None]
        if len(pts) < 3:
            return None, None
        lines[k] = fit(pts)
        kk, cc = lines[k]
        resid = max(resid, max(abs(u - (kk * v + cc)) for v, u in pts))

    def corner(side, cap):
        (kv, cv), (kh, ch) = lines[side], lines[cap]
        y = (kh * cv + ch) / (1 - kh * kv)
        return kv * y + cv, y

    quad = [corner("L", "T"), corner("R", "T"), corner("R", "B"), corner("L", "B")]
    angles = {k: math.degrees(math.atan(lines[k][0])) for k in lines}
    angles["resid"] = resid
    return quad, angles


def quad_area(q):
    return abs(sum(q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1] for i in range(4))) / 2


def is_glass(f):
    """Display glass is a clean quadrilateral: straight edges (fit residual
    <= 1.5 px) and a pixel area matching its quad (rounded corners shave a
    little off). Canopy panes are curved or triangular and fail both."""
    return (f["angles"]["resid"] <= 1.5 and
            0.93 <= f["area"] / max(1.0, quad_area(f["quad"])) <= 1.02)


def analyze(path):
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    alpha = img.getchannel("A").load()
    found = holes(alpha, w, h)
    for f in found:
        f["quad"], f["angles"] = measure_quad(alpha, w, h, f["bbox"])
    found = [f for f in found if f["quad"]]
    for f in found:
        if not is_glass(f):
            print(f"  skipped (canopy, not glass): bbox {f['bbox']} area {f['area']} "
                  f"fit resid {f['angles']['resid']:.1f}px fill {f['area'] / max(1.0, quad_area(f['quad'])):.2f}")
    found = [f for f in found if is_glass(f)]
    found.sort(key=lambda f: -f["area"])
    mfds = sorted(found[:3], key=lambda f: f["bbox"][0])
    small = found[3:]
    banner = min(small, key=lambda f: f["bbox"][1]) if small else None
    rest = sorted([f for f in small if f is not banner], key=lambda f: f["bbox"][0])
    order = [("Left", mfds[0] if len(mfds) > 0 else None),
             ("Center", mfds[1] if len(mfds) > 1 else None),
             ("Right", mfds[2] if len(mfds) > 2 else None),
             ("Banner", banner),
             ("SetSpeed", rest[0] if len(rest) > 0 else None),
             ("Velocity", rest[-1] if len(rest) > 1 else None)]

    print(f"{path}: {w}x{h}, {len(found)} display holes")
    for name, f in order:
        if not f:
            print(f"  {name:9s} (none)")
            continue
        a = f["angles"]
        print(f"  {name:9s} bbox {f['bbox']} area {f['area']}  "
              f"edges L{a['L']:+.1f} R{a['R']:+.1f} T{a['T']:+.1f} B{a['B']:+.1f} deg")
    print("\n  // display rows (Display enum order):")
    for name, f in order:
        if not f:
            print("  {},")
            continue
        q = f["quad"]
        print("  { { " + ", ".join("{ %.1ff, %.1ff }" % p for p in q) + " } },  // " + name)

    # Boresight: along the centre column, between the arch underside and the
    # dashboard top, 45% of the way down (upper-middle of the canopy).
    col = w // 2
    y = h // 2
    while y > 0 and alpha[col, y] < 128:
        y -= 1
    arch = y
    y = h // 2
    while y < h - 1 and alpha[col, y] < 128:
        y += 1
    dash = y
    print(f"\n  canopy along centre column: arch underside y={arch}, dash top y={dash}")
    print(f"  suggested boresight_y = {arch + 0.45 * (dash - arch):.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("finalize")
    f.add_argument("src")
    f.add_argument("out")
    f.add_argument("--key", help="RRGGBB chroma key for RGB input (omit for native alpha)")
    f.add_argument("--pad-top", type=int, default=0, metavar="N",
                   help="mirror N ceiling rows above the canvas (glass too close to the top)")
    a = sub.add_parser("analyze")
    a.add_argument("art")
    args = ap.parse_args()
    if args.cmd == "finalize":
        key = tuple(int(args.key[i:i + 2], 16) for i in (0, 2, 4)) if args.key else None
        finalize(args.src, args.out, key, args.pad_top)
    else:
        analyze(args.art)


if __name__ == "__main__":
    main()
