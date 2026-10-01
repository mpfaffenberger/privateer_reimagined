"""A still patron: one held pose with very small moves (#578).

The merc guild woman holds her pose; over the loop she only fiddles with her
nails (rendered: render_patrons._still rocks her hand bones for one cycle),
glances left and right, smiles and blinks. Her rig has no face or eye
bones and her UVs are auto-unwrapped confetti, so the face moves are made
here, on the rendered frames:

    glance      pixels inside each eye opening shift sideways (the iris
                moves between the lids)
    faces       AI expression frames (#601): an image model repaints her
                face, rendered `scale` times sharper (render_patrons.py
                --face), with a smile or closed eyes. Each is brought down
                to plate px, checked to register with the render, and
                blended in through feathered regions, 0..1 as it fades

The room file's `still` block (plate px, seconds):

    period_s   the loop
    fidget     {cycle_s, at: [[start, length]], bones}: when the rendered
               fidget cycle plays (lengths are whole cycles)
    glances    [[start, hold, -1 left | 1 right]], shift_px, eyes: [[cx, cy, rx, ry]]
    faces      {scale, box: [x0, y0, x1, y1], feather_px, expressions:
                {name: {src (in the room's sources/), regions: [[cx, cy, rx, ry]],
                at: [[start, rise, hold, fall]]}}}: later expressions
                blend over earlier ones

    smoke      {puffs: [{at, from: [x, y]}], particles, emit_s, life_s,
                velocity: [vx, vy] px/s, rise_px_s, drift_px_s, wobble_px,
                radius_px: [start, end], alpha, color, seed}: exhaled
               smoke (the merchant, #579), its own layer over the patron

Every slot shows one composition of (fidget frame, glance, expression levels);
each distinct one is built once and packed once (write_sheet).
"""
import numpy as np
from PIL import Image
from scipy import ndimage

LEVELS = 6              # fade steps: the rise and fall share them
# An expression frame's mean |diff| from the render away from its regions,
# 0-255 (bake_patrons' clean-plate check). The model repaints everything, so
# a frame that moved or rescaled her face shows up here; a good one is ~2.
MAX_FACE_ERROR = 6.0


def glance(rgba, eyes, dx):
    """`rgba` with what's inside each eye opening nudged `dx` px to screen
    right. Each row inside an ellipse is resampled with its ends held, so a
    corner never pulls in skin; the lids and face stay put."""
    out = rgba.copy()
    for cx, cy, rx, ry in eyes:
        for y in range(int(cy - ry), int(np.ceil(cy + ry)) + 1):
            t = 1.0 - ((y - cy) / ry) ** 2
            if t <= 0.0:
                continue
            x0, x1 = int(np.ceil(cx - rx * t ** 0.5)), int(cx + rx * t ** 0.5)
            if x1 - x0 < 2:
                continue
            xs = np.arange(x0, x1 + 1, dtype=float)
            for c in range(4):
                row = rgba[y, x0:x1 + 1, c].astype(float)
                out[y, x0:x1 + 1, c] = np.interp(xs - dx, xs, row).round()   # clamps at the ends
    return out


def load_faces(spec, sources, rest):
    """-> {name: (rgb, weight)}, crop-sized float arrays over `box`: each
    expression frame at plate px and where it's blended in (its regions,
    feathered, only where the render `rest` is opaque, so her outline
    never changes). Exits if a frame doesn't register with `rest`."""
    x0, y0, x1, y1 = spec["box"]
    face = rest[y0:y1, x0:x1].astype(float)
    opaque = ndimage.binary_erosion(face[..., 3] == 255, iterations=1)
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(float)
    layers = {}
    for name, expr in spec["expressions"].items():
        src = Image.open(sources / expr["src"]).convert("RGB")
        rgb = np.asarray(src.resize((x1 - x0, y1 - y0), Image.LANCZOS), float)
        inside = np.zeros(yy.shape, bool)
        for cx, cy, rx, ry in expr["regions"]:
            inside |= ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2 <= 1.0
        away = opaque & ~ndimage.binary_dilation(inside, iterations=3)
        error = np.abs(rgb - face[..., :3])[away].mean()
        if error > MAX_FACE_ERROR:
            raise SystemExit(f"{expr['src']} doesn't register with the render (error {error:.1f})")
        layers[name] = (rgb, ndimage.gaussian_filter(inside.astype(float), spec["feather_px"]) * opaque)
    return layers


def wear(rgba, box, layers, levels):
    """`rgba` with each (name, level) expression in `levels` blended in, in
    order."""
    x0, y0, x1, y1 = box
    face = rgba[y0:y1, x0:x1, :3].astype(float)
    for name, k in levels:
        rgb, weight = layers[name]
        a = k * weight[..., None]
        face = face * (1.0 - a) + rgb * a
    out = rgba.copy()
    out[y0:y1, x0:x1, :3] = np.clip(face, 0, 255).round()
    return out


def smoke(spec, fps, period):
    """-> (sprites, slots): puffs of smoke, as (sprite, dst) frames on the
    loop (slots without smoke draw nothing). Each puff is `particles` soft
    Gaussian splats born over emit_s at `from`: they leave at `velocity`
    (slowing), rise and drift, grow and fade over life_s. Seeded, so a bake
    is reproducible. Straight alpha: the colour is flat, the alpha is the
    smoke."""
    rng = np.random.default_rng(spec["seed"])
    life, (r0, r1) = spec["life_s"], spec["radius_px"]
    parts = []              # (birth s, x0, y0, vx, vy, alpha scale, sway phase, sway Hz)
    for puff in spec["puffs"]:
        for _ in range(spec["particles"]):
            vx, vy = np.array(spec["velocity"]) * rng.uniform(0.6, 1.4, 2)
            parts.append((puff["at"] + rng.uniform(0, spec["emit_s"]), *puff["from"],
                          vx, vy, rng.uniform(0.5, 1.0), rng.uniform(0, 2 * np.pi),
                          rng.uniform(0.3, 0.8)))
    sprites, slots = [], []
    for slot in range(period):
        t = slot / fps
        blobs = []
        for born, x0, y0, vx, vy, scale, phase, hz in parts:
            age = (t - born) % (period / fps)        # wraps: a puff may cross the loop
            if age >= life:
                continue
            u = age / life
            ease = 1.0 - np.exp(-age / 0.5)          # the exhale slows down
            # Each splat sways on its own phase, more as it rises: wisps, not a lump.
            sway = spec.get("wobble_px", 0.0) * u * np.sin(2 * np.pi * hz * age + phase)
            x = x0 + vx * 0.5 * ease + spec["drift_px_s"] * age + sway
            y = y0 + vy * 0.5 * ease - spec["rise_px_s"] * age
            fade = min(1.0, age / 0.15) * (1.0 - u) ** 2
            blobs.append((x, y, r0 + (r1 - r0) * u, spec["alpha"] * scale * fade))
        if not blobs:
            continue
        pad = 3 * max(r for _, _, r, _ in blobs)
        bx0 = int(min(x for x, _, _, _ in blobs) - pad)
        by0 = int(min(y for _, y, _, _ in blobs) - pad)
        bx1 = int(max(x for x, _, _, _ in blobs) + pad) + 1
        by1 = int(max(y for _, y, _, _ in blobs) + pad) + 1
        yy, xx = np.mgrid[by0:by1, bx0:bx1].astype(float)
        clear = np.ones_like(xx)                     # 1 - alpha, splat by splat
        for x, y, r, a in blobs:
            clear *= 1.0 - a * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * r * r))
        alpha = np.clip((1.0 - clear) * 255, 0, 255).round().astype(np.uint8)
        if alpha.max() < 3:
            continue
        rgba = np.zeros((*alpha.shape, 4), np.uint8)
        rgba[..., :3], rgba[..., 3] = spec["color"], alpha
        sprites.append((rgba, [bx0, by0, bx1 - bx0, by1 - by0]))
        slots.append(slot)
    return sprites, slots


def _level(t, windows):
    """0..1 at time `t`: eased up over rise, held, eased down over fall."""
    for start, rise, hold, fall in windows:
        u = t - start
        if 0.0 <= u < rise:
            k = u / rise
        elif rise <= u < rise + hold:
            k = 1.0
        elif rise + hold <= u < rise + hold + fall:
            k = 1.0 - (u - rise - hold) / fall
        else:
            continue
        return round((3 * k * k - 2 * k * k * k) * LEVELS) / LEVELS   # smoothstep
    return 0.0


def timeline(still, fps, frames, bake, sources=None):
    """-> (sprites, slots, period). `frames`: the rendered fidget cycle
    (plate-sized RGBA; frames[0] is the rest pose; bit-identical frames
    may be one array). `bake`: RGBA -> (sprite, dst), the patron's
    clip-and-trim. `sources`: the room's sources/ (expression frames)."""
    period = round(still["period_s"] * fps)
    faces = still.get("faces")
    layers = load_faces(faces, sources, frames[0]) if faces else {}
    cache, shown = {}, []
    for slot in range(period):
        t = slot / fps
        pose = next((round((t - start) * fps) % len(frames)
                     for start, length in still.get("fidget", {}).get("at", [])
                     if start <= t < start + length), 0)
        look = next((d for start, hold, d in still.get("glances", [])
                     if start <= t < start + hold), 0)
        levels = tuple((name, k) for name, expr in (faces or {}).get("expressions", {}).items()
                       if (k := _level(t, expr["at"])))
        key = (id(frames[pose]), look, levels)      # identical frames share an array
        if key not in cache:
            rgba = frames[pose]
            if levels:
                rgba = wear(rgba, faces["box"], layers, levels)
            if look:
                rgba = glance(rgba, still["eyes"], look * still["shift_px"])
            cache[key] = bake(rgba)
        shown.append(cache[key])
    return shown, list(range(period)), period
