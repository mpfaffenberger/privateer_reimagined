"""reskin_decals.py — per-ship planar decals for tools/reskin_ship_blender.py.

Decals are projected in object space (the raw OBJ frame) and composited
into the base colour BEFORE the weathering layer, so panel seams, grime
and AO age them like real paint."""

from dataclasses import dataclass, replace
from pathlib import Path

import bpy
import mathutils

from reskin_nodes import add_node, math_op, vec_op

DECAL_DIR = Path(__file__).resolve().parents[1] / "assets" / "meshes" / "decals"


@dataclass(frozen=True)
class Decal:
    """A planar decal painted into the base colour before weathering, so
    seams / grime / AO age it like real paint. All vectors are in the raw
    OBJ frame (= Blender object space here, and the lights3d.json frame)."""
    image:  str                         # file under assets/meshes/decals/
    center: tuple[float, float, float]
    normal: tuple[float, float, float]  # surface normal the decal faces along
    up:     tuple[float, float, float]  # image top points this way
    width:  float                       # square decals: width == height
    depth:  float = 25.0                # projection half-thickness along normal
    mirror_x: bool = False              # also paint the X-mirrored twin

    def instances(self):
        yield self
        if self.mirror_x:
            flip = lambda v: (-v[0], v[1], v[2])
            yield replace(self, center=flip(self.center), normal=flip(self.normal),
                          up=flip(self.up), mirror_x=False)


# Per-ship decals. Poses were measured by ray-casting the posed ship in
# Blender for the largest fully-flat patch (method: issue #476).
DECALS: dict[str, tuple[Decal, ...]] = {
    # Demon (bounty hunter): skull-and-crosshair on each wing top, skull
    # crown toward the nose (native frame: nose −Y, top +Z).
    "demon": (Decal("bounty_skull.png", center=(264.0, 208.0, 8.1),
                    normal=(0.053, -0.002, 0.999), up=(0.0, -1.0, 0.0),
                    width=96.0, mirror_x=True),),
}


def _paint_decal(t, color, texco, decal: Decal, y: float):
    """Composite one decal over `color`; returns the new colour socket."""
    n = mathutils.Vector(decal.normal).normalized()
    up = mathutils.Vector(decal.up).normalized()
    right = up.cross(n).normalized()          # image +U; keeps art un-mirrored
    local = vec_op(t, "SUBTRACT", texco.outputs["Object"], tuple(decal.center), (-1400, y)).outputs[0]
    axis = lambda vec, dy: vec_op(t, "DOT_PRODUCT", local, tuple(vec), (-1200, y + dy)).outputs["Value"]
    u = math_op(t, "MULTIPLY_ADD", axis(right, 0), 1.0 / decal.width, 0.5, location=(-1000, y))
    v = math_op(t, "MULTIPLY_ADD", axis(up, -150), 1.0 / decal.width, 0.5, location=(-1000, y - 150))
    uv = add_node(t, "ShaderNodeCombineXYZ", (-800, y))
    t.links.new(u, uv.inputs["X"])
    t.links.new(v, uv.inputs["Y"])

    img = bpy.data.images.load(str(DECAL_DIR / decal.image), check_existing=True)
    img.alpha_mode = "STRAIGHT"
    tex = add_node(t, "ShaderNodeTexImage", (-600, y), image=img, extension="CLIP")
    t.links.new(uv.outputs[0], tex.inputs["Vector"])

    # Only surfaces near the decal plane AND facing its way get paint.
    near = math_op(t, "LESS_THAN", math_op(t, "ABSOLUTE", axis(n, -300), location=(-1000, y - 300)),
                 decal.depth, location=(-800, y - 300))
    facing = math_op(t, "GREATER_THAN", vec_op(t, "DOT_PRODUCT", texco.outputs["Normal"], tuple(n),
                                           (-1200, y - 450)).outputs["Value"], 0.5, location=(-800, y - 450))
    fac = math_op(t, "MULTIPLY", math_op(t, "MULTIPLY", tex.outputs["Alpha"], near, location=(-400, y - 200)),
                facing, location=(-200, y - 200))
    # color + (decal - color) * fac
    delta = vec_op(t, "SUBTRACT", tex.outputs["Color"], color, (-400, y)).outputs[0]
    scaled = vec_op(t, "SCALE", delta, location=(-200, y))
    t.links.new(fac, scaled.inputs["Scale"])
    return vec_op(t, "ADD", color, scaled.outputs[0], (0, y)).outputs[0]


def build_decal_group(ship: str):
    """Color in → Color out with every decal for `ship` composited on top.
    Returns None when the ship has no decals (callers skip the node)."""
    decals = [inst for d in DECALS.get(ship, ()) for inst in d.instances()]
    if not decals:
        return None
    ng = bpy.data.node_groups.new("ReskinDecals", "ShaderNodeTree")
    ng.interface.new_socket("Color", in_out="INPUT", socket_type="NodeSocketColor")
    ng.interface.new_socket("Color", in_out="OUTPUT", socket_type="NodeSocketColor")
    gin, gout = add_node(ng, "NodeGroupInput", (-1800, 0)), add_node(ng, "NodeGroupOutput", (300, 0))
    texco = add_node(ng, "ShaderNodeTexCoord", (-1800, -300))
    color = gin.outputs["Color"]
    for i, decal in enumerate(decals):
        color = _paint_decal(ng, color, texco, decal, y=-i * 700)
    ng.links.new(color, gout.inputs["Color"])
    return ng
