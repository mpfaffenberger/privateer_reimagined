"""Procedural pedestrian proxies for any camera-matched room (#515; shared in
#586). Runs inside Blender.

Deliberately low-detail: walkers are 30-100 px tall on screen, so
silhouette, swing and rim light sell them, not modelling detail. Bigger
than that (New Detroit's 90-160 px, #590), boxes read as walking crates:
build_rigged_walker() walks one of the rigged characters in characters/
instead, with a procedural walk cycle on their shared Mixamo skeleton.
"""
import math

import bpy
from mathutils import Quaternion, Vector

from base import TOOLS
from stage import animate_path, box_object, material

CHARACTERS = TOOLS / "characters"
RIGGED_STEP_M = 0.62      # the step rigged_gait's default stride takes


def shaped_box(name, size, loc, mat, bevel=0.0, parent=None, nose_taper=1.0, nose_drop=0.0,
               base_flare=1.0):
    """Box with an optional wedge nose (+Y verts narrowed by `nose_taper`, top
    +Y verts lowered by `nose_drop` x height) and `base_flare` (bottom verts
    widened in X and Y, e.g. a coat hem)."""
    obj = box_object(name, size, loc, mat, bpy.context.scene.collection)
    for v in obj.data.vertices:
        if v.co.y > 0.0:
            v.co.x *= nose_taper
            if v.co.z > 0.0:
                v.co.z -= nose_drop * size[2]
        if v.co.z < 0.0:
            v.co.x *= base_flare
            v.co.y *= base_flare
    if bevel > 0.0:
        mod = obj.modifiers.new("Bevel", 'BEVEL')
        mod.width, mod.segments = bevel, 3
    for poly in obj.data.polygons:
        poly.use_smooth = True
    if parent is not None:
        obj.parent = parent
    return obj


def _pivot(name, loc, parent):
    empty = bpy.data.objects.new(name, None)
    empty.location, empty.parent = loc, parent
    bpy.context.scene.collection.objects.link(empty)
    return empty


def build_walker(name, coat_rgb, trousers_rgb=(0.05, 0.05, 0.06), skin_rgb=(0.45, 0.32, 0.25),
                 head_rgb=None):
    """A ~1.75 m pedestrian proxy facing +Y, origin at the feet. Returns
    (root, limbs) where limbs are the four swing pivots. `head_rgb` covers
    the head (a bandana, a hood); by default it's bare skin."""
    root = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(root)
    coat = material(f"{name}Coat", coat_rgb, roughness=0.7)
    legs = material(f"{name}Legs", trousers_rgb, roughness=0.8)
    skin = material(f"{name}Skin", skin_rgb, roughness=0.6)
    head = material(f"{name}Head", head_rgb, roughness=0.7) if head_rgb else skin

    # Overcoat from shoulders to knees, narrower at the waist than the
    # shoulders and flaring to the hem: reads as a person, not a crate.
    shaped_box("Chest", (0.44, 0.25, 0.36), (0, 0, 1.36), coat, bevel=0.07, parent=root,
               base_flare=0.82)
    shaped_box("Coat", (0.36, 0.22, 0.72), (0, 0, 0.84), coat, bevel=0.05, parent=root,
               base_flare=1.3)
    shaped_box("Head", (0.18, 0.2, 0.23), (0, 0.01, 1.68), head, bevel=0.085, parent=root)
    limbs = []
    for side, x in (("L", -0.09), ("R", 0.09)):
        hip = _pivot(f"{name}Hip{side}", (x, 0, 0.93), root)
        shaped_box(f"Leg{side}", (0.12, 0.14, 0.9), (0, 0, -0.45), legs, bevel=0.04, parent=hip)
        shoulder = _pivot(f"{name}Shoulder{side}", (x * 2.4, 0, 1.5), root)
        shaped_box(f"Arm{side}", (0.1, 0.11, 0.6), (0, 0, -0.3), coat, bevel=0.03,
                   parent=shoulder)
        limbs.append((hip, shoulder))
    return root, limbs


def animate_walk_path(root, limbs, points, z, fps, speed=1.3, step_m=0.72, swing_deg=24.0):
    """Walk the polyline `points` [(x, y), ...] on a floor at height `z`,
    beginning at frame 1 and turning smoothly through corners. Duration and
    gait both follow from `speed`, so the feet never slide. Returns the last
    frame."""
    stride_hz = speed / (2.0 * step_m)                      # one cycle = two steps

    def phase(f):
        return 2.0 * math.pi * stride_hz * (f - 1) / fps

    frame_end = animate_path(root, points, z, fps, speed,
                             lift=lambda f: 0.025 * abs(math.sin(phase(f))))
    for f in range(1, frame_end + 1):
        for i, (hip, shoulder) in enumerate(limbs):
            swing = math.radians(swing_deg) * math.sin(phase(f) + math.pi * i)
            hip.rotation_euler = (swing, 0.0, 0.0)
            shoulder.rotation_euler = (-0.8 * swing, 0.0, 0.0)
            hip.keyframe_insert("rotation_euler", frame=f)
            shoulder.keyframe_insert("rotation_euler", frame=f)
    return frame_end


def animate_walk(root, limbs, start, end, z, fps, speed=1.3, step_m=0.72, swing_deg=24.0):
    """Walk in a straight line from `start` to `end` (x, y). Returns the last
    frame."""
    return animate_walk_path(root, limbs, [start, end], z, fps, speed, step_m, swing_deg)


def build_rigged_walker(name, model, height=1.75):
    """A rigged character from characters/<model> with its clip removed,
    scaled to `height` and turned to face +Y, origin at the feet. The clips
    are idles and sits, so no paid walk clip: see rigged_gait().
    Returns (root, gait)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(CHARACTERS / model))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == 'ARMATURE')
    # Rest pose: arms down, standing. Clearing the clip leaves its last
    # evaluated pose on every bone (shoulders hunched forward), so reset them.
    arm.animation_data_clear()
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion, pb.location, pb.scale = (1, 0, 0, 0), (0, 0, 0), (1, 1, 1)
    for obj in new:
        if obj.type == 'MESH' and not any(m.type == 'ARMATURE' for m in obj.modifiers):
            obj.hide_render = obj.hide_viewport = True   # the importer's bone shape
    root = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(root)
    top = (arm.matrix_world @ arm.data.bones["mixamorig:HeadTop_End"].head_local).z
    rig = bpy.data.objects.new(f"{name}Rig", None)   # the model faces -Y, feet at 0
    bpy.context.scene.collection.objects.link(rig)
    rig.parent, rig.rotation_euler = root, (0.0, 0.0, math.pi)
    rig.scale = (height / top,) * 3
    for obj in new:
        if obj.parent is None:
            obj.parent = rig
    return root, rigged_gait(arm)


def rigged_gait(arm, stride_deg=24.0, knee_deg=55.0, arm_deg=14.0):
    """A procedural walk cycle on the Mixamo skeleton: a function (phase,
    frame) that keys it. Every swing is about the model's side axis (world X
    in its rest frame), converted into each bone's rest frame, so bone roll
    doesn't matter and parents carry children. The model faces -Y, so
    "forward" (toward -Y) is a negative angle."""
    bones = arm.pose.bones
    side, fwd, up = Vector((1.0, 0.0, 0.0)), Vector((0.0, 1.0, 0.0)), Vector((0.0, 0.0, 1.0))

    def turn(bone, *parts):
        """Pose `bone` as the product of (world axis, degrees) rotations."""
        rest = arm.data.bones[f"mixamorig:{bone}"].matrix_local.to_3x3().inverted()
        q = Quaternion()
        for world_axis, deg in parts:
            q = q @ Quaternion((rest @ world_axis).normalized(), math.radians(deg))
        pb = bones[f"mixamorig:{bone}"]
        pb.rotation_quaternion = q
        return pb

    def pose(phase, frame):
        posed = []
        for s, offset in (("Left", 0.0), ("Right", math.pi)):
            p = phase + offset
            hip = stride_deg * math.sin(p) + 4.0            # + = thigh forward
            # The knee folds while the leg swings through (thigh moving forward).
            knee = 6.0 + knee_deg * max(0.0, math.cos(p - 0.35)) ** 2
            ankle = 0.6 * (knee - hip) - 4.0                 # keeps the sole near level
            posed.append(turn(f"{s}UpLeg", (side, -hip)))
            posed.append(turn(f"{s}Leg", (side, knee)))
            posed.append(turn(f"{s}Foot", (side, -ankle)))
            swing = -arm_deg * math.sin(p)                   # arms oppose the legs
            tuck = 9.0 if s == "Left" else -9.0              # A-pose arms to the sides
            posed.append(turn(f"{s}Arm", (fwd, tuck), (side, -swing)))
            posed.append(turn(f"{s}ForeArm", (side, -(12.0 + 0.4 * max(0.0, swing)))))
        # The pelvis sways with the stride and drops at double support; the
        # torso counter-turns and leans a touch into the walk.
        hips = turn("Hips", (up, 5.0 * math.sin(phase)))
        hips.location = (0.0, -0.02 * abs(math.sin(phase)), 0.0)   # bone Y is world up
        hips.keyframe_insert("location", frame=frame)
        posed += [hips, turn("Spine", (side, -3.0), (up, -4.0 * math.sin(phase)))]
        for pb in posed:
            pb.keyframe_insert("rotation_quaternion", frame=frame)
    return pose


def animate_rigged_walk(root, gait, points, z, fps, speed=1.3, step_m=RIGGED_STEP_M):
    """A rigged walker along the polyline `points` (see stage.animate_path;
    `z` may be a floor-height function), its gait keyed in step with
    `speed`, so the feet don't slide. Returns the last frame."""
    frame_end = animate_path(root, points, z, fps, speed)
    stride_hz = speed / (2.0 * step_m)                      # one cycle = two steps
    for f in range(1, frame_end + 1):
        gait(2.0 * math.pi * stride_hz * (f - 1) / fps, f)
    return frame_end
