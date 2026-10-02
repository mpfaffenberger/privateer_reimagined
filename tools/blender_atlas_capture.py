"""Blender half of render_blender_sprite_atlas.py (#699): render one frame per
view of a job file and project the job's 3D feature lights into each frame.

    blender -b ship.blend --python tools/blender_atlas_capture.py -- job.json

The job (written by render_blender_sprite_atlas.py) carries every camera pose
already solved by render_3d_sprite_atlases.camera_for_orbit, so the orbit and
elevation-sign conventions live in exactly one place. This file only maps the
engine frame (+Y up, +Z nose) into Blender's (x, -z, y), a pure rotation, so a
pose here sees exactly what the engine camera would.

Writes <raw_dir>/<cell>.png per view and <raw_dir>/projections.json:
{cell: [{"px", "py", "visible"}, ...]} with one entry per light, in frame
pixels (x right, y down).
"""
import json
import math
import os
import sys

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Matrix, Vector

# A light counts as facing the camera down to this dot product, so exhaust
# glows sitting on a nozzle rim still peek out in a pure side view. The ray
# cast below is what actually hides lights behind the hull.
FACING_SLACK = -0.15


def to_blender(v):
    x, y, z = v
    return Vector((x, -z, y))


def normalise(scene, length):
    """Centre the hull's bounding box on the origin and scale its longest
    side to `length`, like the engine's `length_meters`. Returns the matrix
    applied so the feature lights can follow the hull."""
    meshes = [o for o in scene.objects if o.type == "MESH" and not o.hide_render]
    pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    m = Matrix.Scale(length / max(hi - lo), 4) @ Matrix.Translation(-(lo + hi) / 2)
    for o in scene.objects:
        if o.parent is None and o.type not in {"CAMERA", "LIGHT"}:
            o.matrix_world = m @ o.matrix_world
    return m


def setup_scene(scene, job):
    """Studio look of the engine's --capture-clean mode: transparent film,
    one world-fixed sun, flat ambient floor. Lights saved in the .blend are
    preview-only and stay out of the atlas."""
    for o in scene.objects:
        if o.type == "LIGHT":
            o.hide_render = True
    r = scene.render
    r.engine = "BLENDER_EEVEE"
    r.film_transparent = True
    r.resolution_x = r.resolution_y = job["resolution"]
    r.resolution_percentage = 100
    r.image_settings.file_format = "PNG"
    r.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "Standard"

    world = bpy.data.worlds.new("atlas_ambient")
    bg = world.node_tree.nodes["Background"]
    a = job["ambient"]
    bg.inputs["Color"].default_value = (a, a, a, 1.0)
    scene.world = world

    sun = bpy.data.objects.new("atlas_sun", bpy.data.lights.new("atlas_sun", "SUN"))
    sun.data.energy = job["sun_energy"]
    toward_sun = to_blender(job["sun_dir"]).normalized()
    sun.rotation_euler = (-toward_sun).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(sun)

    cam = bpy.data.objects.new("atlas_cam", bpy.data.cameras.new("atlas_cam"))
    cam.data.lens_unit = "FOV"
    cam.data.angle = math.radians(job["fov_deg"])
    cam.data.clip_end = 10.0 * job["radius_m"]
    scene.collection.objects.link(cam)
    scene.camera = cam
    return cam


def pose_camera(cam, x, y, z, yaw, pitch):
    """The engine's yaw/pitch camera (no roll), so the polar caps keep the
    same image 'up' the engine capture used."""
    yw, p = math.radians(yaw), math.radians(pitch)
    fwd = to_blender((-math.sin(yw) * math.cos(p), math.sin(p), -math.cos(yw) * math.cos(p)))
    up = to_blender((math.sin(yw) * math.sin(p), math.cos(p), math.cos(yw) * math.sin(p)))
    rot = Matrix((fwd.cross(up), up, -fwd)).transposed()
    cam.matrix_world = Matrix.Translation(to_blender((x, y, z))) @ rot.to_4x4()


def project(scene, cam, lights, m, step):
    """Frame pixel and visibility of every light for the current pose:
    in front of the camera, facing it, and not hidden behind the hull.
    Rays start `step` off the hull so they don't hit the light's own face."""
    deps = bpy.context.evaluated_depsgraph_get()
    size = scene.render.resolution_x
    eye = cam.matrix_world.translation
    out = []
    for light in lights:
        p = m @ to_blender(light["pos"])
        n = (m.to_3x3() @ to_blender(light["normal"])).normalized()
        to_eye = eye - (p + n * step)
        hit = scene.ray_cast(deps, p + n * step, to_eye.normalized(),
                             distance=to_eye.length)[0]
        ndc = world_to_camera_view(scene, cam, p)
        facing = n.dot(to_eye.normalized()) > FACING_SLACK
        out.append({"px": ndc.x * size, "py": (1.0 - ndc.y) * size,
                    "visible": bool(ndc.z > 0 and facing and not hit)})
    return out


def main():
    with open(sys.argv[sys.argv.index("--") + 1]) as f:
        job = json.load(f)
    scene = bpy.context.scene
    lights = []
    if job.get("lights"):
        with open(job["lights"]) as f:
            lights = json.load(f)["lights"]
    cam = setup_scene(scene, job)
    m = normalise(scene, job["length_m"])
    projections = {}
    for view in job["views"]:
        pose_camera(cam, *view["cam"])
        bpy.context.view_layer.update()
        scene.render.filepath = os.path.join(job["raw_dir"], view["file"])
        bpy.ops.render.render(write_still=True)
        projections[view["file"]] = project(scene, cam, lights, m,
                                            step=0.005 * job["length_m"])
    with open(os.path.join(job["raw_dir"], "projections.json"), "w") as f:
        json.dump(projections, f, indent=1)


if __name__ == "__main__":
    main()
