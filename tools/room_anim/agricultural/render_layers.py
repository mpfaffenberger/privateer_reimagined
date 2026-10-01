"""Render the raw passes for the Agricultural concourse atrium's walkers (#605).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/agricultural/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer walker_bridge --frames 1:12 --samples 16     # quick look

--check renders the decks, holdouts and routes (with a 1.75 m post every
3 m) from the matched camera over the plate: build/room_anim/agricultural/check.png.

Each route in routes.py is one layer: a rigged character from characters/
on walkers.py's procedural gait. Passes (see ../render.py) go to
build/room_anim/agricultural/<layer>/; bake_layer.py --base agricultural then
turns them into plate-aware RGBA sprites. (The window traffic is
render_traffic.py's.)
"""
import argparse
import sys
from pathlib import Path

import bpy

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import camera_match as cm  # noqa: E402
import render  # noqa: E402
import routes  # noqa: E402
import scene as atrium  # noqa: E402
import walkers  # noqa: E402
from base import paths  # noqa: E402
from stage import overlay_on_plate  # noqa: E402

AGRICULTURAL = paths("agricultural")
WALK_FPS = 12      # enough for a 45-105 px figure, and half the atlas of 24 fps
POST_EVERY_M = 3.0


def _build_atrium():
    sc = atrium.reset()
    atrium.setup_render(sc)
    cam = atrium.add_plate_camera(sc)
    atrium.attach_plate_reference(cam, str(AGRICULTURAL.plate))
    decks = atrium.add_decks(sc)
    atrium.add_occluders(sc)
    atrium.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_atrium()
    route = routes.ROUTES[layer]
    sc.render.fps = WALK_FPS
    root, gait = walkers.build_rigged_walker(layer, route["model"])
    last = walkers.animate_rigged_walk(root, gait, routes.path(layer), route["z"], WALK_FPS,
                                       route["speed"])
    sc.frame_start, sc.frame_end = 1, last
    return sc, decks, [root]


def check(out_png):
    """Decks, holdouts and routes from the matched camera, over the plate."""
    sc = atrium.reset()
    atrium.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    atrium.add_plate_camera(sc)
    glow = {c: atrium.material(f"Guide{c}", (0, 0, 0), emission=rgb, strength=0.5)
            for c, rgb in (("deck", (0.2, 0.5, 1.0)), ("hold", (1.0, 0.2, 0.8)),
                           ("path", (0.2, 1.0, 0.3)), ("post", (1.0, 0.9, 0.1)))}
    for obj in atrium.add_decks(sc):
        obj.data.materials[0] = glow["deck"]
        obj.display_type = 'WIRE'
        obj.modifiers.new("Wire", 'WIREFRAME').thickness = 0.06
    for obj in atrium.add_occluders(sc):
        obj.is_holdout = False
        obj.data.materials[0] = glow["hold"]
    # The fitted circles: the kiosk platform's rim, the lower ring's kerb and
    # the outer edge of its tiles.
    for z, radius in ((cm.DECK_Z, cm.PLATFORM_R), (cm.LOWER_Z, cm.KERB_R),
                      (cm.LOWER_Z, routes.TILES_EDGE_R)):
        for deg in range(0, 360, 2):
            atrium.box_object(f"Ring{radius}_{deg}", (0.12, 0.12, 0.02),
                              (*cm.around_axis(radius, deg), z + 0.01), glow["deck"],
                              sc.collection)
    for name, route in routes.ROUTES.items():
        z, walked = route["z"], 0.0
        points = routes.path(name)
        for i, (x, y) in enumerate(points):
            atrium.box_object(f"{name}Path{i}", (0.15, 0.15, 0.02), (x, y, z + 0.01),
                              glow["path"], sc.collection)
            if i:
                walked += ((x - points[i - 1][0]) ** 2 + (y - points[i - 1][1]) ** 2) ** 0.5
            if walked >= POST_EVERY_M or i == 0:
                walked = 0.0
                atrium.box_object(f"{name}Post{i}", (0.3, 0.3, 1.75), (x, y, z + 0.875),
                                  glow["post"], sc.collection)
    guides = out_png.with_name("check_guides.png")
    sc.render.filepath = str(guides)
    bpy.ops.render.render(write_still=True)
    overlay_on_plate(AGRICULTURAL.plate, guides, out_png)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="decks/holdouts/routes over the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(routes.ROUTES)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    AGRICULTURAL.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(AGRICULTURAL.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(routes.ROUTES) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, AGRICULTURAL.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
