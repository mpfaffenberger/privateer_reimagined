"""A still patron: one held pose with very small moves (#578).

The merc guild woman holds her pose; over the loop she only fiddles with her
nails (rendered: render_patrons._still rocks her hand bones for one cycle),
glances left and right, and smiles a little. Her rig has no face or eye
bones and her UVs are auto-unwrapped confetti, so the face moves are made
here, in screen space, on the rendered frames:

    glance      pixels inside each eye opening shift sideways (the iris
                moves between the lids)
    smile       a smooth displacement field lifts her mouth corners and
                lower lids, scaled 0..1 as it fades in and out

The room file's `still` block (plate px, seconds):

    period_s   the loop
    fidget     {cycle_s, at: [[start, length]], bones}: when the rendered
               fidget cycle plays (lengths are whole cycles)
    glances    [[start, hold, -1 left | 1 right]], shift_px, eyes: [[cx, cy, rx, ry]]
    smile      {corners: [[x, y]], lift_px, widen_px, lids: [[x, y]], lid_px,
                sigma_px, at: [[start, rise, hold, fall]]}

Every slot shows one composition of (fidget frame, glance, smile level);
each distinct one is built once and packed once (write_sheet).
"""
import numpy as np
from scipy import ndimage

SMILE_LEVELS = 6        # fade steps: the rise and fall share them


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


def smile(rgba, spec, k):
    """`rgba` warped a fraction `k` of the way into a small smile:
    Gaussian bumps move the mouth corners up by lift_px and out by
    widen_px, and the lower lids up by lid_px (smiling eyes), sub-pixel.
    Only a box around the face is resampled."""
    mid = sum(x for x, _ in spec["corners"]) / len(spec["corners"])
    bumps = [(x, y, np.sign(x - mid) * spec.get("widen_px", 0.0), spec["lift_px"])
             for x, y in spec["corners"]] + \
            [(x, y, 0.0, spec["lid_px"]) for x, y in spec["lids"]]
    sigma, reach = spec["sigma_px"], 3 * spec["sigma_px"]
    xs0 = int(min(b[0] for b in bumps) - reach)
    xs1 = int(max(b[0] for b in bumps) + reach) + 1
    ys0 = int(min(b[1] for b in bumps) - reach)
    ys1 = int(max(b[1] for b in bumps) + reach) + 1
    yy, xx = np.mgrid[ys0:ys1, xs0:xs1].astype(float)
    move_x, move_up = np.zeros_like(xx), np.zeros_like(xx)
    for x, y, out_px, up_px in bumps:
        g = np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma ** 2))
        move_x += out_px * g
        move_up += up_px * g
    out = rgba.copy()
    # Sample where the content comes from: below, and nearer the middle.
    coords = [yy - ys0 + k * move_up, xx - xs0 - k * move_x]
    for c in range(4):
        plane = rgba[ys0:ys1, xs0:xs1, c].astype(float)
        out[ys0:ys1, xs0:xs1, c] = np.clip(
            ndimage.map_coordinates(plane, coords, order=3, mode="nearest"), 0, 255).round()
    return out


def _smile_level(t, windows):
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
        return round((3 * k * k - 2 * k * k * k) * SMILE_LEVELS) / SMILE_LEVELS   # smoothstep
    return 0.0


def timeline(still, fps, frames, bake):
    """-> (sprites, slots, period). `frames`: the rendered fidget cycle
    (plate-sized RGBA; frames[0] is the rest pose). `bake`: RGBA ->
    (sprite, dst), the patron's clip-and-trim."""
    period = round(still["period_s"] * fps)
    cache, shown = {}, []
    for slot in range(period):
        t = slot / fps
        pose = next((round((t - start) * fps) % len(frames)
                     for start, length in still.get("fidget", {}).get("at", [])
                     if start <= t < start + length), 0)
        look = next((d for start, hold, d in still.get("glances", [])
                     if start <= t < start + hold), 0)
        k = _smile_level(t, still["smile"]["at"]) if "smile" in still else 0.0
        key = (pose, look, k)
        if key not in cache:
            rgba = frames[pose]
            if k:
                rgba = smile(rgba, still["smile"], k)
            if look:
                rgba = glance(rgba, still["eyes"], look * still["shift_px"])
            cache[key] = bake(rgba)
        shown.append(cache[key])
    return shown, list(range(period)), period
