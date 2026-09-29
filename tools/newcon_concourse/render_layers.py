"""Render the raw passes for one animated New Con concourse layer (#515).

Run inside Blender (headless):
    blender --background --factory-startup \
        --python tools/newcon_concourse/render_layers.py -- --layer all
    ... -- --layer walker_toward --frames 1:24     # quick look
    ... -- --review-blend                          # regenerate the review .blend

Writes to build/newcon_concourse/<layer>/ (NNNN = frame number, from 1;
off-screen frames are skipped and become blank timeline slots):
    pass.json        {"frames": N, "fps": F, "exposure_ev": E} for the full pass
    empty.png        the proxy deck with no actor (rendered once)
    beauty/NNNN.png  actor over the proxy deck, rendered only inside a border
                     around the actor and its floor footprint (alpha 0 = not
                     rendered = "this frame cannot change the plate here")
    mask/NNNN.png    actor coverage, deck held out      (alpha only matters)
bake_layer.py then turns these into plate-aware RGBA sprites.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import actors  # noqa: E402
import scene as hall  # noqa: E402

REPO = HERE.parents[1]
PLATE = REPO / "assets/concourse/newcon/concourse_bg.png"
BUILD = REPO / "build/newcon_concourse"
REVIEW_BLEND = HERE / "newcon_concourse.blend"


def _car_receding(sc):
    car = actors.build_hover_car("CarReceding")
    actors.animate_straight_pass(car, x=-9.0, y_start=11.5, y_end=150.0, hover_z=0.9,
                                 frame_start=1, frame_end=240)
    return [car], (1, 240)


def _car_approaching(sc):
    """Kerb lane between the walkway (X -25) and the red stripe (X -21.6),
    coming toward the camera headlights-first and leaving frame-left (fully
    off-screen by Y~20 m; the cart holdout covers its last metres)."""
    car = actors.build_hover_car("CarApproaching", hull_rgb=(0.34, 0.29, 0.24))
    actors.animate_straight_pass(car, x=-23.2, y_start=150.0, y_end=19.0, hover_z=1.0,
                                 frame_start=1, frame_end=256, heading_deg=180.0)
    return [car], (1, 256)


WALK_FPS = 12      # plenty for a 30-60 px figure, and half the atlas of 24 fps


def _walker(sc, name, coat_rgb, start, end, speed):
    sc.render.fps = WALK_FPS
    # Walkers only touch the walkway. The glossy lane deck would mirror them
    # past the kerb, where the painted kerb and railings would block it.
    sc.objects["Deck"].hide_render = True
    root, limbs = actors.build_walker(name, coat_rgb)
    last = actors.animate_walk(root, limbs, start, end, hall.PROMENADE_Z, WALK_FPS,
                               speed=speed)
    return [root], (1, last)


def _walker_toward(sc):
    """Steps out from behind the kiosk pod, drifts toward the middle of the
    walkway and walks at the camera, leaving frame-left."""
    return _walker(sc, "WalkerToward", (0.16, 0.10, 0.06), start=(-30.5, 72.0),
                   end=(-28.0, 23.0), speed=1.3)


def _walker_away(sc):
    """Enters frame-left near the kerb and strolls away, easing over to the
    shopfronts and disappearing behind the kiosk pod."""
    return _walker(sc, "WalkerAway", (0.07, 0.09, 0.12), start=(-26.8, 23.0),
                   end=(-30.5, 72.0), speed=1.15)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {
    "car_receding": _car_receding,
    "car_approaching": _car_approaching,
    "walker_toward": _walker_toward,
    "walker_away": _walker_away,
}


def _build_hall():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(PLATE))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_hall()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def save_review_blend(path):
    """One scene with every layer's actors (a collection per layer) over the
    camera-matched hall, for inspecting the setup in Blender. Walker layers
    are keyed at their own 12 fps, so they move at half speed in this 24 fps
    scene. Renders always come from build(), never from this file."""
    sc, _ = _build_hall()
    last = 1
    for name, builder in LAYERS.items():
        before = set(bpy.data.objects)
        _, (_, end) = builder(sc)
        last = max(last, end)
        coll = bpy.data.collections.new(name)
        sc.collection.children.link(coll)
        for obj in set(bpy.data.objects) - before:
            for owner in obj.users_collection:
                owner.objects.unlink(obj)
            coll.objects.link(obj)
    sc.objects["Deck"].hide_render = False       # walker builders hide it
    sc.render.fps, sc.frame_start, sc.frame_end = 24, 1, last
    # Forward-slash relative plate path so the file opens on macOS too.
    rel = Path(os.path.relpath(PLATE, Path(path).parent)).as_posix()
    for img in bpy.data.images:
        img.filepath = f"//{rel}"
    bpy.ops.wm.save_as_mainfile(filepath=str(path), relative_remap=False, compress=True)
    Path(f"{path}1").unlink(missing_ok=True)   # generated file: no .blend1 backup


def _set_decks(sc, decks, mask_pass):
    """Beauty: opaque film, decks visible. Mask: transparent film, decks held
    out so alpha is pure actor coverage."""
    sc.render.film_transparent = mask_pass
    for deck in decks:
        deck.is_holdout = mask_pass


def _set_actor_visibility(roots, visible):
    for root in roots:
        for obj in [root, *root.children_recursive]:
            obj.hide_render = not visible


def set_border(sc, roots, margin=0.35, pad_px=12, min_px=4, footprint=True):
    """Limit rendering to the actor's screen box plus (with `footprint`) its
    footprint on the deck (where reflections and glow land), widened by
    `margin`. Returns False when that box is off-screen, so the frame can be
    skipped. Assumes the camera looks down +Y."""
    cam, pts = sc.camera, []
    for root in roots:
        for obj in root.children_recursive:
            if obj.type != 'MESH':
                continue
            for corner in obj.bound_box:
                world = obj.matrix_world @ Vector(corner)
                ground = (Vector((world.x, world.y, 0.0)),) if footprint else ()
                for p in (world, *ground):
                    if p.y > cam.location.y + 0.5:             # in front of the lens
                        pts.append(world_to_camera_view(sc, cam, p))
    if not pts:
        return False
    x0, x1 = min(p.x for p in pts), max(p.x for p in pts)
    y0, y1 = min(p.y for p in pts), max(p.y for p in pts)
    r = sc.render
    mx = (x1 - x0) * margin + pad_px / r.resolution_x
    my = (y1 - y0) * margin + pad_px / r.resolution_y
    r.use_border, r.use_crop_to_border = True, False
    r.border_min_x, r.border_max_x = max(0.0, x0 - mx), min(1.0, x1 + mx)
    r.border_min_y, r.border_max_y = max(0.0, y0 - my * 2.0), min(1.0, y1 + my)
    return ((r.border_max_x - r.border_min_x) * r.resolution_x >= min_px and
            (r.border_max_y - r.border_min_y) * r.resolution_y >= min_px)


def _render(sc, path):
    sc.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


def render_passes(sc, decks, roots, out_dir, frames=None, mask_samples=16):
    out_dir = Path(out_dir)
    first, last = frames or (sc.frame_start, sc.frame_end)
    beauty_samples = sc.cycles.samples

    sc.render.use_persistent_data = True
    sc.frame_set(first)
    _set_actor_visibility(roots, False)
    sc.render.use_border = False
    _set_decks(sc, decks, mask_pass=False)
    _render(sc, out_dir / "empty.png")
    (out_dir / "pass.json").write_text(json.dumps(
        {"frames": sc.frame_end - sc.frame_start + 1, "fps": sc.render.fps,
         "exposure_ev": sc.view_settings.exposure}) + "\n")
    _set_actor_visibility(roots, True)

    for f in range(first, last + 1):
        sc.frame_set(f)
        if not set_border(sc, roots):
            continue            # off-screen: no files; bake treats it as a gap
        _set_decks(sc, decks, mask_pass=False)
        sc.cycles.samples, sc.cycles.use_denoising = beauty_samples, True
        _render(sc, out_dir / "beauty" / f"{f:04d}.png")
        _set_decks(sc, decks, mask_pass=True)
        sc.cycles.samples, sc.cycles.use_denoising = mask_samples, False
        _render(sc, out_dir / "mask" / f"{f:04d}.png")
    sc.cycles.samples, sc.cycles.use_denoising = beauty_samples, True


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--review-blend", action="store_true",
                    help=f"write {REVIEW_BLEND.relative_to(REPO)} (no rendering)")
    args = ap.parse_args(argv)
    if args.review_blend:
        save_review_blend(REVIEW_BLEND)
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        render_passes(sc, decks, roots, BUILD / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
