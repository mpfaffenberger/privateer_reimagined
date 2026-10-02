"""Procedural pedestrian proxies for any camera-matched room (#515; shared in
#586). Runs inside Blender.

Deliberately low-detail: walkers are 30-100 px tall on screen, so
silhouette, swing and rim light sell them, not modelling detail. Bigger
than that (New Detroit's 90-160 px, #590), boxes read as walking crates:
build_rigged_walker() walks one of the rigged characters in characters/
instead, with a procedural walk cycle on their shared Mixamo skeleton.
"""
import math
from types import SimpleNamespace

import bpy
from mathutils import Quaternion, Vector

from base import TOOLS
from stage import along, animate_path, box_object, material

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


# The Mixamo skeleton's rest frame: the model faces -Y, +X is its left, Z up.
SIDE, FWD, UP = Vector((1.0, 0.0, 0.0)), Vector((0.0, 1.0, 0.0)), Vector((0.0, 0.0, 1.0))


def rest_turn(arm, bone, *parts):
    """`bone`'s pose rotation for the product of (rest-frame axis, degrees)
    rotations, converted into the bone's own rest frame, so bone roll
    doesn't matter and parents carry children."""
    rest = arm.data.bones[f"mixamorig:{bone}"].matrix_local.to_3x3().inverted()
    q = Quaternion()
    for axis, deg in parts:
        q = q @ Quaternion((rest @ axis).normalized(), math.radians(deg))
    return q


def rest_aim(arm, bone, direction):
    """`bone`'s pose rotation that points it along `direction` (rest frame:
    -Y forward, +X the model's left, Z up), parents at rest (#682)."""
    b = arm.data.bones[f"mixamorig:{bone}"]
    axis, angle = (b.tail_local - b.head_local).normalized().rotation_difference(
        Vector(direction).normalized()).to_axis_angle()
    return rest_turn(arm, bone, (axis, math.degrees(angle)))


def rigged_gait(arm, stride_deg=24.0, knee_deg=55.0, arm_deg=14.0):
    """A procedural walk cycle on the Mixamo skeleton: a function (phase,
    frame, amp=1) that keys it. Every swing is about the model's side axis
    (world X in its rest frame, rest_turn). The model faces -Y, so "forward"
    (toward -Y) is a negative angle. `amp` scales every swing (#682): 0 is
    standing, in between a shuffle (starting, stopping, turning on the spot)."""
    bones = arm.pose.bones

    def turn(bone, *parts):
        pb = bones[f"mixamorig:{bone}"]
        pb.rotation_quaternion = rest_turn(arm, bone, *parts)
        return pb

    def pose(phase, frame, amp=1.0):
        posed = []
        for s, offset in (("Left", 0.0), ("Right", math.pi)):
            p = phase + offset
            hip = amp * stride_deg * math.sin(p) + 4.0      # + = thigh forward
            # The knee folds while the leg swings through (thigh moving forward).
            knee = 6.0 + amp * knee_deg * max(0.0, math.cos(p - 0.35)) ** 2
            ankle = 0.6 * (knee - hip) - 4.0                 # keeps the sole near level
            posed.append(turn(f"{s}UpLeg", (SIDE, -hip)))
            posed.append(turn(f"{s}Leg", (SIDE, knee)))
            posed.append(turn(f"{s}Foot", (SIDE, -ankle)))
            swing = -amp * arm_deg * math.sin(p)             # arms oppose the legs
            tuck = 9.0 if s == "Left" else -9.0              # A-pose arms to the sides
            posed.append(turn(f"{s}Arm", (FWD, tuck), (SIDE, -swing)))
            posed.append(turn(f"{s}ForeArm", (SIDE, -(12.0 + 0.4 * max(0.0, swing)))))
        # The pelvis sways with the stride and drops at double support; the
        # torso counter-turns and leans a touch into the walk.
        hips = turn("Hips", (UP, amp * 5.0 * math.sin(phase)))
        hips.location = (0.0, -amp * 0.02 * abs(math.sin(phase)), 0.0)   # bone Y is world up
        hips.keyframe_insert("location", frame=frame)
        posed += [hips, turn("Spine", (SIDE, -3.0), (UP, -amp * 4.0 * math.sin(phase)))]
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


# A guided tour (#682): walk to a spot, turn, point something out, walk on.
TURN_DEG_S = 150.0           # turning on the spot
WALK_TURN_DEG_S = 240.0      # heading change while walking
ACCEL_S = 0.45               # standing to full speed, and back
RAISE_S, LOWER_S = 0.6, 0.5  # the pointing arm
SHUFFLE_AMP = 0.3            # the gait's swing while turning on the spot
SHUFFLE_STEP_S = 0.45        # one shuffle step
SHUFFLE_IN_S = 0.25          # the shuffle's fade in


def _wrap(a):
    return (a + math.pi) % (2.0 * math.pi) - math.pi


def heading_to(x0, y0, x1, y1):
    """Heading (rad about Z, 0 = facing +Y) from (x0, y0) toward (x1, y1)."""
    return math.atan2(-(x1 - x0), y1 - y0)


def _ease(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def _turn_toward(heading, want, max_step):
    return heading + max(-max_step, min(max_step, _wrap(want - heading)))


def plan_tour(start, heading, beats, fps, speed=1.1, step_m=RIGGED_STEP_M):
    """One loop of a guided tour from `start` (x, y) facing `heading` (rad),
    as a state per frame. Beats, in order:
        {"walk": [(x, y), ...]}  through these points, easing in and out;
        {"present": (x, y, z), "hold": s, "face": rad}  turn on the spot to
            `face` (default: toward the point), raise the nearer arm at it,
            hold, lower.
    A state is x, y, heading, phase (gait), amp (gait swing), arm ("Left",
    "Right" or None), weight (the arm's, 0-1) and target (x, y, z). The
    gait's phase advances with the distance walked, so the feet don't slide.
    Pure Python; key_tour() keys it."""
    dt = 1.0 / fps
    s = SimpleNamespace(x=start[0], y=start[1], heading=heading, phase=0.0)
    states = []

    def emit(amp, arm=None, weight=0.0, target=None):
        states.append(SimpleNamespace(**vars(s), amp=amp, arm=arm, weight=weight, target=target))

    for beat in beats:
        if "walk" in beat:
            pts = [(s.x, s.y), *beat["walk"]]
            length = sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))
            done, t, decel = 0.0, 0.0, speed / ACCEL_S
            while done < length - 1e-4:
                t += dt
                v = min(speed, speed * t / ACCEL_S, math.sqrt(2.0 * decel * (length - done)))
                ds = min(max(v, 0.1 * speed) * dt, length - done)
                done += ds
                s.x, s.y = along(pts, done)
                ax, ay = along(pts, done + 0.4)
                if math.dist((ax, ay), (s.x, s.y)) > 1e-3:
                    s.heading = _turn_toward(s.heading, heading_to(s.x, s.y, ax, ay),
                                             math.radians(WALK_TURN_DEG_S) * dt)
                s.phase += math.pi * ds / step_m           # one cycle = two steps
                emit(ds / dt / speed)
            continue
        target = tuple(beat["present"])
        face = beat.get("face", heading_to(s.x, s.y, *target[:2]))
        t = 0.0
        while abs(err := _wrap(face - s.heading)) > math.radians(0.5):
            t += dt
            s.heading = _turn_toward(s.heading, face, math.radians(TURN_DEG_S) * dt)
            s.phase += math.pi * dt / SHUFFLE_STEP_S
            emit(SHUFFLE_AMP * min(1.0, t / SHUFFLE_IN_S, abs(err) / math.radians(20.0)))
        # A + heading turns left (toward -X when facing +Y): the point is on his left.
        arm = "Left" if _wrap(heading_to(s.x, s.y, *target[:2]) - s.heading) > 0.0 else "Right"
        raise_n, hold_n, lower_n = (max(1, round(sec * fps))
                                    for sec in (RAISE_S, beat["hold"], LOWER_S))
        for i in range(raise_n):
            emit(0.0, arm, _ease((i + 1) / raise_n), target)
        for _ in range(hold_n):
            emit(0.0, arm, 1.0, target)
        for i in range(lower_n):
            emit(0.0, arm, _ease(1.0 - (i + 1) / lower_n), target)
    return states


def key_tour(root, gait, states, shoulder_z=1.45, look_deg=55.0, lift_deg=15.0):
    """Key plan_tour()'s `states` from frame 1 on a build_rigged_walker():
    the root, the gait, the pointing arm (straightened, aimed `lift_deg`
    above the target so it reads as a gesture, not a hanging arm; blended
    in by weight) and the head turned toward it. Returns the last frame."""
    arm = next(o for o in root.children_recursive if o.type == 'ARMATURE')
    bpy.context.view_layer.update()
    # The armature's rotation under the root (the rig's half turn and the
    # importer's own), for turning world directions into its rest frame.
    rel = (root.matrix_world.inverted() @ arm.matrix_world).to_3x3().normalized().to_quaternion()
    bones = arm.pose.bones
    for f, st in enumerate(states, start=1):
        root.location, root.rotation_euler = (st.x, st.y, 0.0), (0.0, 0.0, st.heading)
        root.keyframe_insert("location", frame=f)
        root.keyframe_insert("rotation_euler", frame=f)
        gait(st.phase, f, st.amp)
        look = Quaternion()
        if st.arm:
            to_rest = (Quaternion(UP, st.heading) @ rel).inverted()
            tx, ty, tz = st.target
            d = to_rest @ Vector((tx - st.x, ty - st.y, tz - shoulder_z))
            d.z += d.xy.length * math.tan(math.radians(lift_deg))
            for bone, goal in ((f"{st.arm}Arm", rest_aim(arm, f"{st.arm}Arm", d)),
                               (f"{st.arm}ForeArm", Quaternion())):
                pb = bones[f"mixamorig:{bone}"]
                pb.rotation_quaternion = pb.rotation_quaternion.slerp(goal, st.weight)
                pb.keyframe_insert("rotation_quaternion", frame=f)
            yaw = math.degrees(math.atan2(d.x, -d.y))    # + = toward the model's left
            look = rest_turn(arm, "Head", (UP, st.weight * max(-look_deg, min(look_deg, yaw))))
        pb = bones["mixamorig:Head"]
        pb.rotation_quaternion = look
        pb.keyframe_insert("rotation_quaternion", frame=f)
    return len(states)
