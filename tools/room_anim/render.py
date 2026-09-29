"""Shared Blender render passes for plate-aware room layers (#515; split out
in #557). Runs inside Blender. A base's render script builds its
camera-matched scene and actors, then calls render_passes().

Writes to <out_dir>/ (NNNN = frame number, from 1; off-screen frames are
skipped and become blank timeline slots):
    pass.json        {"frames": N, "fps": F, "exposure_ev": E} for the full pass
    empty.png        the proxy deck with no actor (rendered once)
    beauty/NNNN.png  actor over the proxy deck, rendered only inside a border
                     around the actor and its floor footprint (alpha 0 = not
                     rendered = "this frame cannot change the plate here")
    mask/NNNN.png    actor coverage, deck held out      (alpha only matters)
bake_layer.py then turns these into plate-aware RGBA sprites.
"""
import json
from pathlib import Path

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector


def _set_decks(sc, decks, mask_pass):
    """Beauty: opaque film, decks visible. Mask: transparent film, decks held
    out so alpha is pure actor coverage."""
    sc.render.film_transparent = mask_pass
    for deck in decks:
        deck.is_holdout = mask_pass


def set_actor_visibility(roots, visible):
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
    set_actor_visibility(roots, False)
    sc.render.use_border = False
    _set_decks(sc, decks, mask_pass=False)
    _render(sc, out_dir / "empty.png")
    (out_dir / "pass.json").write_text(json.dumps(
        {"frames": sc.frame_end - sc.frame_start + 1, "fps": sc.render.fps,
         "exposure_ev": sc.view_settings.exposure}) + "\n")
    set_actor_visibility(roots, True)

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
