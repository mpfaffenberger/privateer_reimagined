"""Render the raw passes for the animated mining concourse (#558).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/mining/render_layers.py -- --check
    ... -- --layer all

--check renders glowing floor guides (the guide strip's edges, cross lines
every 5 m, lines along the tunnel) from the matched camera and overlays them
on the plate: build/room_anim/mining/check.png. If the camera match is right
they hug the painted strip and run parallel to the painted grates.

Layers write their passes (see ../render.py) to build/room_anim/mining/<layer>/;
bake_layer.py --base mining then turns them into plate-aware RGBA sprites.
"""
import argparse
import sys
from pathlib import Path

import bpy

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import actors  # noqa: E402
import render  # noqa: E402
import scene as hall  # noqa: E402
from base import paths  # noqa: E402

MINING = paths("mining")
TRAIN_SPEED = 3.0          # m/s, a loaded ore tug's crawl


def _ore_train(sc):
    """In from the far lift doors along the guide strip, headlights first,
    and out past the bottom-right of the frame."""
    tug, hopper, beacon = actors.build_ore_train()
    y_start, y_end = 55.0, -2.0
    frames = round((y_start - y_end) / TRAIN_SPEED * sc.render.fps)
    actors.animate_train(tug, hopper, beacon, hall.GUIDE_X, y_start, y_end, 1, frames,
                         heading_deg=180.0, fps=sc.render.fps)
    return [tug, hopper], (1, frames)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {"ore_train": _ore_train}


def _build_tunnel():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(MINING.plate))
    decks = hall.add_deck(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_tunnel()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def check(out_png):
    """Floor guides from the matched camera, overlaid on the plate."""
    sc = hall.reset()
    hall.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    hall.add_plate_camera(sc)
    glow = {c: hall.material(f"Guide{c}", (0, 0, 0), emission=rgb, strength=0.9)
            for c, rgb in (("strip", (1.0, 0.2, 0.2)), ("cross", (0.2, 0.7, 1.0)),
                           ("axis", (0.3, 1.0, 0.3)))}
    for edge in (-0.5, 0.5):                                  # guide strip edges
        hall.box_object(f"Strip{edge}", (0.1, 120.0, 0.01),
                        (hall.GUIDE_X + edge * hall.GUIDE_WIDTH, 62.0, 0.0),
                        glow["strip"], sc.collection)
    for y in range(5, 80, 5):                                 # cross lines
        hall.box_object(f"Cross{y}", (30.0, 0.05, 0.01), (0.0, float(y), 0.0),
                        glow["cross"], sc.collection)
    for x in (-6.0, -2.0, 8.0, 12.0):                         # lines down the tunnel
        hall.box_object(f"Axis{x}", (0.05, 120.0, 0.01), (x, 62.0, 0.0),
                        glow["axis"], sc.collection)
    tmp = MINING.build / "check_guides.png"
    sc.render.filepath = str(tmp)
    bpy.ops.render.render(write_still=True)

    plate = bpy.data.images.load(str(MINING.plate))
    guides = bpy.data.images.load(str(tmp))
    w, h = plate.size
    p, g = list(plate.pixels), list(guides.pixels)
    for i in range(0, len(p), 4):                            # guides over the plate
        a = g[i + 3]
        for c in range(3):
            p[i + c] = g[i + c] * a + p[i + c] * (1.0 - a)
    out = bpy.data.images.new("check", w, h, alpha=True)
    out.pixels = p
    out.filepath_raw, out.file_format = str(out_png), 'PNG'
    out.save()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="floor-guide overlay on the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    MINING.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(MINING.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, MINING.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
