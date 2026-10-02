"""Shared Blender scene helpers for camera-matched room plates (#515; shared
in #558). Runs inside Blender; each base's scene.py builds its own camera,
decks, occluders and lights on top of these.

Pure bpy, no add-ons, no bpy.ops for geometry: works identically in the live
MCP session and `blender --background`.
"""
import math

import bmesh
import bpy
import numpy as np

PLATE_W, PLATE_H = 1536, 1024

# Passes render 2 stops dark so lamp hot spots keep headroom in 8-bit PNGs.
# A clipped channel turns a shadow into a colour shift (a shadow on a
# red-clipped floor only loses green/blue and prints red). bake_layer.py
# reads this back from pass.json and undoes it in linear light.
EXPOSURE_EV = -2.0


def reset():
    bpy.ops.wm.read_homefile(use_empty=True)
    return bpy.context.scene


def setup_render(scene, samples=48):
    scene.render.engine = 'CYCLES'
    scene.render.resolution_x, scene.render.resolution_y = PLATE_W, PLATE_H
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False     # toggled per pass by render.py
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = 0.3
    scene.render.fps = 24
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '8'    # PIL reads 16-bit RGBA as 8 anyway
    scene.view_settings.view_transform = 'Standard'   # plain sRGB, like the plate
    scene.view_settings.exposure = EXPOSURE_EV
    cyc = scene.cycles
    cyc.samples = samples
    cyc.use_denoising = True
    cyc.seed = 515          # fixed seed: A and B share noise, so A-B cancels it
    cyc.max_bounces = 6
    # CUDA, not OptiX: Blender 5.2's OptiX kernel fails to compile on the dev
    # box's driver (OPTIX_ERROR_INTERNAL_COMPILER_ERROR). CPU is the fallback.
    prefs = bpy.context.preferences.addons['cycles'].preferences
    try:
        prefs.compute_device_type = 'CUDA'
        prefs.get_devices()
        for dev in prefs.devices:
            dev.use = dev.type == 'CUDA'
        cyc.device = 'GPU' if any(d.use for d in prefs.devices) else 'CPU'
    except (TypeError, ValueError):
        cyc.device = 'CPU'


def attach_plate_reference(cam, plate_path):
    """Show the painting behind the camera in the viewport (not rendered)."""
    img = bpy.data.images.load(plate_path, check_existing=True)
    cam.data.show_background_images = True
    bg = cam.data.background_images.new()
    bg.image, bg.alpha, bg.frame_method = img, 1.0, 'STRETCH'


def box_object(name, size, loc, mat, collection):
    """A box mesh object built through the data API."""
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=size, verts=bm.verts)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    obj.location = loc
    collection.objects.link(obj)
    return obj


def material(name, base, metallic=0.0, roughness=0.5, emission=None, strength=0.0,
             transmission=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*base, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Transmission Weight"].default_value = transmission
    if emission is not None:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1.0)
        bsdf.inputs["Emission Strength"].default_value = strength
    return mat


def emitter(name, parent, loc, radius, rgb, strength):
    """A small glowing sphere (lamp glass, nav light) parented to `parent`."""
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=radius)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(material(name, (0.0, 0.0, 0.0), emission=rgb, strength=strength))
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.parent, obj.location = parent, loc
    return obj


def animate_straight_pass(obj, x, y_start, y_end, hover_z, frame_start, frame_end,
                          bob=0.06, heading_deg=0.0):
    """Constant-speed pass along Y with a gentle vertical bob (hover, or
    engine rumble). Every frame is keyed, so interpolation mode is
    irrelevant."""
    obj.rotation_euler = (0.0, 0.0, math.radians(heading_deg))
    span = frame_end - frame_start
    for f in range(frame_start, frame_end + 1):
        t = (f - frame_start) / span
        obj.location = (x, y_start + (y_end - y_start) * t,
                        hover_z + bob * math.sin(f * 0.35))
        obj.keyframe_insert("location", frame=f)


def along(points, dist):
    """(x, y) at `dist` metres along the polyline `points`, clamped to it."""
    for i, ((x0, y0), (x1, y1)) in enumerate(zip(points, points[1:])):
        seg = math.hypot(x1 - x0, y1 - y0)
        if dist <= seg or i == len(points) - 2:
            t = min(max(dist / seg, 0.0), 1.0) if seg else 0.0
            return x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
        dist -= seg
    return points[-1]


def animate_path(obj, points, z, fps, speed, frame_start=1, turn_m=0.8, lift=None):
    """Constant-speed trip along the polyline `points` [(x, y), ...] at
    height `z` (or a function (x, y) -> height: a kerb, a platform), keyed
    every frame from `frame_start`. The heading (+Y forward) follows the
    path over `turn_m` metres, so corners turn smoothly; `lift(frame)` adds
    a vertical offset (a gait, a hover bob). Returns the last frame."""
    length = sum(math.hypot(x1 - x0, y1 - y0)
                 for (x0, y0), (x1, y1) in zip(points, points[1:]))
    frame_end = frame_start + round(length / speed * fps)
    heading = None
    for f in range(frame_start, frame_end + 1):
        dist = length * (f - frame_start) / (frame_end - frame_start)
        x, y = along(points, dist)
        (ax, ay), (bx, by) = along(points, dist - turn_m / 2), along(points, dist + turn_m / 2)
        want = math.atan2(-(bx - ax), by - ay)
        if heading is not None:                     # the short way round, no spins
            want = heading + (want - heading + math.pi) % (2.0 * math.pi) - math.pi
        heading = want
        floor = z(x, y) if callable(z) else z
        obj.location = (x, y, floor + (lift(f) if lift else 0.0))
        obj.rotation_euler = (0.0, 0.0, heading)
        obj.keyframe_insert("location", frame=f)
        obj.keyframe_insert("rotation_euler", frame=f)
    return frame_end


def _rgba(img):
    """An image's float pixels as an (N, 4) float64 array."""
    px = np.empty(len(img.pixels), dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.astype(np.float64).reshape(-1, 4)


def overlay_on_plate(plate_path, guides_path, out_png):
    """Alpha-over a straight-alpha render (camera-match guides) onto the
    plate and save it: a scene's `--check` image. Blends in float64, as the
    per-pixel Python loop it replaced did (#617), so the PNG is unchanged."""
    plate = bpy.data.images.load(str(plate_path))
    guides = bpy.data.images.load(str(guides_path))
    w, h = plate.size
    p, g = _rgba(plate), _rgba(guides)
    a = g[:, 3:]
    p[:, :3] = g[:, :3] * a + p[:, :3] * (1.0 - a)
    out = bpy.data.images.new("check", w, h, alpha=True)
    out.pixels.foreach_set(p.astype(np.float32).ravel())
    out.filepath_raw, out.file_format = str(out_png), 'PNG'
    out.save()


def light(scene, name, kind, loc, color, energy, size=1.0, rot=(0.0, 0.0, 0.0)):
    data = bpy.data.lights.new(name, kind)
    data.color, data.energy = color, energy
    if kind == 'AREA':
        data.size = size
    elif kind in ('POINT', 'SPOT'):
        data.shadow_soft_size = size
    obj = bpy.data.objects.new(name, data)
    obj.location, obj.rotation_euler = loc, rot
    scene.collection.objects.link(obj)
    return obj


def spot(name, parent, loc, rot, rgb, energy, angle_deg, blend=0.4):
    """A small spot lamp parented to `parent` (a headlight, a beacon). Spots
    shine down local -Z; `rot` aims them."""
    obj = light(bpy.context.scene, name, 'SPOT', loc, rgb, energy, size=0.08, rot=rot)
    obj.data.spot_size, obj.data.spot_blend = math.radians(angle_deg), blend
    obj.parent = parent
    return obj
