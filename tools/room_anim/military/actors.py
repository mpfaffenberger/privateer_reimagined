"""The Military concourse's munitions train (#588): a yellow tow tug pulling an
ordnance trailer down the vehicle lane, the original game's beat here (the
legacy concourse_car overlay: a tug towing a flatbed of munitions), rebuilt
from the original base-vehicle meshes.

    tug      ships_wcnews/truck.obj  yellow tow tug
    trailer  ships_wcnews/cart.obj   the wheeled hopper, painted gunmetal and
                                     racked with missiles (trailer.obj is a
                                     wheelless pod: it floated over the lane)

Unlike the mining ore train (mining/actors.py), this one turns a corner: it
comes out from under the teal ramp at the lane's far end. Both vehicles
follow one path by arc length, so the trailer tracks the tug through the bend.
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector

import ships
from stage import emitter, material

TUG_LENGTH, TRAILER_LENGTH = 3.0, 3.0       # m; the hopper is ~0.73 as wide as long
COUPLING_GAP = 0.5                          # m between tug and trailer
HEADLIGHT = (1.0, 0.88, 0.7)
BEACON_AMBER = (1.0, 0.45, 0.08)
TUG_GRIME = (0.85, 0.8, 0.72)               # keep the yellow, knock off the shine
TRAILER_PAINT = (0.3, 0.33, 0.28)           # the stock hopper is near-white: gunmetal
MISSILE_WHITE, MISSILE_BAND, MISSILE_NOSE = (0.6, 0.61, 0.58), (0.55, 0.05, 0.03), (0.1, 0.1, 0.1)


class Route:
    """A polyline with its corners rounded to `fillet` m arcs, sampled by arc
    length; beyond either end it runs on straight."""

    def __init__(self, points, fillet, step=0.05):
        pts = [Vector((x, y)) for x, y in points]
        dense = [pts[0]]
        for a, b, c in zip(pts, pts[1:], pts[2:]):
            d_in, d_out = (b - a).normalized(), (c - b).normalized()
            turn = math.acos(max(-1.0, min(1.0, d_in.dot(d_out))))
            cut = fillet * math.tan(turn / 2)
            p0, p1 = b - d_in * cut, b + d_out * cut
            n = max(2, int(fillet * turn / step))
            for i in range(n + 1):          # quadratic Bezier ~ the arc, tangent at both ends
                t = i / n
                dense.append(p0 * (1 - t) ** 2 + b * 2 * t * (1 - t) + p1 * t * t)
        dense.append(pts[-1])
        self.pts = dense
        self.s = [0.0]
        for p, q in zip(dense, dense[1:]):
            self.s.append(self.s[-1] + (q - p).length)
        self.length = self.s[-1]

    def at(self, s):
        """-> (x, y, heading_deg) at arc length s; heading turns +Y (nose) onto
        the direction of travel."""
        i = max(0, min(len(self.s) - 2, next((k for k, v in enumerate(self.s) if v > s),
                                              len(self.s)) - 1))
        p, q = self.pts[i], self.pts[i + 1]
        seg = self.s[i + 1] - self.s[i]
        d = (q - p) / seg if seg > 0 else Vector((0.0, 1.0))
        pos = p + d * (s - self.s[i])
        return pos.x, pos.y, math.degrees(math.atan2(-d.x, d.y))


def _spot(name, parent, loc, rot, rgb, energy, angle_deg, blend=0.4):
    data = bpy.data.lights.new(name, 'SPOT')
    data.color, data.energy = rgb, energy
    data.spot_size, data.spot_blend, data.shadow_soft_size = math.radians(angle_deg), blend, 0.08
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    obj.parent, obj.location, obj.rotation_euler = parent, loc, rot
    return obj


def _missile(name, parent, loc, length, radius, mats):
    """A missile lying along Y, nose +Y: body, a red band, a dark nose cone."""
    body, band, nose = mats
    parts = ((0.0, length * 0.78, radius, radius, body),              # (y0, len, r0, r1, mat)
             (length * 0.52, length * 0.08, radius * 1.02, radius * 1.02, band),
             (length * 0.78, length * 0.22, radius, radius * 0.08, nose))
    for i, (y0, span, r0, r1, mat) in enumerate(parts):
        mesh = bpy.data.meshes.new(f"{name}{i}")
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=True, segments=16, radius1=r0, radius2=r1, depth=span)
        # Cones run along Z; -90 deg about X lays them along +Y.
        bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0),
                         matrix=Matrix.Rotation(-math.pi / 2, 3, 'X'))
        bm.to_mesh(mesh)
        bm.free()
        mesh.materials.append(mat)
        obj = bpy.data.objects.new(f"{name}{i}", mesh)
        bpy.context.scene.collection.objects.link(obj)
        obj.parent = parent
        obj.location = Vector(loc) + Vector((0.0, y0 + span / 2 - length / 2, 0.0))


def _missile_rack(parent, size):
    """Five missiles racked in the hopper, three below and two on top."""
    width, length, height = size
    mats = (material("Missile", MISSILE_WHITE, roughness=0.45),
            material("MissileBand", MISSILE_BAND, roughness=0.5),
            material("MissileNose", MISSILE_NOSE, metallic=0.4, roughness=0.4))
    r = width * 0.11
    rows = ((3, height * 0.8 + r), (2, height * 0.8 + r * 2.7))
    for n, z in rows:
        for i in range(n):
            x = (i - (n - 1) / 2) * r * 2.1
            _missile(f"Missile{n}{i}", parent, (x, 0.0, z), length * 0.92, r, mats)


def build_munitions_train():
    """-> (tug root, trailer root, beacon). Both face +Y (nose), origin on the floor."""
    tug = ships.import_ship("truck", TUG_LENGTH, "Tug", grounded=True, tint=TUG_GRIME)
    w, length, h = tug["size"]
    nose = Vector((0.0, length / 2, h * 0.45))
    for side in (-1, 1):
        lamp = nose + Vector((side * w * 0.3, 0.0, 0.0))
        emitter(f"Headlamp{side}", tug, lamp, 0.07, HEADLIGHT, 6.0)
        # Spots shine down local -Z; +80 deg about X aims them forward (+Y)
        # and 10 deg down onto the floor ahead.
        _spot(f"Headlight{side}", tug, lamp + Vector((0, 0.1, 0)),
              (math.radians(80.0), 0.0, 0.0), HEADLIGHT, 30.0, 50.0)
    beacon_at = Vector((0.0, -length * 0.1, h * 1.05))
    emitter("BeaconGlass", tug, beacon_at, 0.09, BEACON_AMBER, 5.0)
    beacon = _spot("Beacon", tug, beacon_at, (math.radians(60.0), 0.0, 0.0),
                   BEACON_AMBER, 60.0, 40.0, blend=0.6)
    trailer = ships.import_ship("cart", TRAILER_LENGTH, "Trailer", grounded=True,
                                tint=TRAILER_PAINT)
    _missile_rack(trailer, trailer["size"])
    return tug, trailer, beacon


def animate_train(tug, trailer, beacon, route, speed, frame_start, frame_end, fps,
                  beacon_rpm=40.0):
    """Drive the tug along `route` from its start at `speed` m/s; the trailer
    follows the coupling behind it on the same route; the beacon sweeps round."""
    trail = (TUG_LENGTH + TRAILER_LENGTH) / 2 + COUPLING_GAP
    last = {}
    for f in range(frame_start, frame_end + 1):
        s = speed * (f - frame_start) / fps
        rumble = 0.008 * math.sin(f * 0.35)      # ground vehicles jiggle, not bob
        for obj, at in ((tug, s), (trailer, s - trail)):
            x, y, heading = route.at(at)
            # Unwrap: +-180 deg flips would motion-blur into a full spin.
            prev = last.get(obj.name, heading)
            heading += 360.0 * round((prev - heading) / 360.0)
            last[obj.name] = heading
            obj.location = (x, y, rumble)
            obj.rotation_euler = (0.0, 0.0, math.radians(heading))
            obj.keyframe_insert("location", frame=f)
            obj.keyframe_insert("rotation_euler", frame=f)
        turn = 2 * math.pi * beacon_rpm / 60.0 * (f - frame_start) / fps
        beacon.rotation_euler = (math.radians(60.0), 0.0, turn)
        beacon.keyframe_insert("rotation_euler", frame=f)
