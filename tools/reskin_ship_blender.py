"""reskin_ship_blender.py — bake richer textures onto a flat-shaded wcnews ship.

Some wcnews meshes (Demon, Orion) were authored with mostly flat 1x1 tint
materials and no UVs on those faces, so in-engine they read as plastic
toys next to the properly textured ships. This headless Blender script
gives them a proper skin:

  1. Import `ships_wcnews/<ship>.obj` + its `materials.json` sidecar.
  2. Join everything into one mesh; Smart-UV-project a fresh "atlas" UV
     (the original UVs survive as "orig", used only as a bake source).
  3. Push every source material (bitmap OR flat tint) through ONE shared
     detail node group: rectilinear panel plating + grooves, per-panel
     value jitter, grime noise, and ambient occlusion — all in object
     space, so faces that never had UVs get detail too.
  4. Bake three atlas maps with Cycles: diffuse (AO baked in), tangent-
     space normal, and spec.
  5. Export `assets/meshes/ships_reskinned/<ship>.obj` + `materials.json`
     + PNGs. Import and export share Blender's default axis settings, so
     the OBJ comes back in the exact source frame and every orientation
     override / lights3d file keyed to the ship stays valid.

Output lives in its own directory so re-running `import_3ds_meshes.py`
never clobbers it; `regenerate_mesh_showroom.mesh_obj_asset()` prefers it
when present.

Usage (Blender 4.2+ / 5.x). `--python-exit-code 1` matters: without it
Blender exits 0 even when the script aborts.
  blender -b --factory-startup --python-exit-code 1 --python tools/reskin_ship_blender.py -- --ship dd_tug
  blender -b --factory-startup --python-exit-code 1 --python tools/reskin_ship_blender.py -- --ship demon --size 1024
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import bpy
import numpy as np

REPO    = Path(__file__).resolve().parents[1]
SRC_DIR = REPO / "assets" / "meshes" / "ships_wcnews"
DST_DIR = REPO / "assets" / "meshes" / "ships_reskinned"

ORIG_UV   = "orig"    # source UVs (bitmaps only) — bake input
ATLAS_UV  = "atlas"   # fresh unwrap — bake output + exported UVs
FINAL_MAT = "RESKIN"  # single material the exported OBJ references

# Materials whose name contains one of these keywords keep a clean
# surface (no panel lines / grime). Canopy glass with rivets looks silly.
NO_DETAIL_KEYWORDS = ("GLASS", "LIGHT")
SHINY_KEYWORDS     = ("GLASS", "SHINY")


@dataclass(frozen=True)
class Look:
    """Every art-direction knob in one place. Scales are per ship length,
    so the same look reads identically on a 700u Demon and a 900u Orion."""
    panels_per_length: float = 12.0   # coarse hull plates along the long axis
    panel_randomness:  float = 0.35   # 0 = perfect grid, 1 = jumbled "cracks"
    fine_panel_ratio:  float = 2.7    # fine plating = coarse × this
    groove_width:      float = 0.06   # panel seam width (cell units)
    groove_darken:     float = 0.60   # colour multiplier loss inside seams
    fine_weight:       float = 0.40   # fine seams relative to coarse ones
    panel_jitter:      float = 0.20   # ± per-panel brightness variation
    grime_scale:       float = 5.0    # grime blotches per ship length
    grime_darken:      float = 0.40
    ao_distance:       float = 0.04   # fraction of ship length
    ao_floor:          float = 0.35   # darkest AO multiplier
    base_spec:         float = 0.45
    shiny_spec:        float = 0.95
    bump_distance:     float = 0.0015 # groove depth, fraction of ship length
    orig_bump_weight:  float = 0.6    # how much the source bitmaps' bump survives
    authored_detail:   float = 0.3    # procedural detail strength over real bitmaps
                                      # (the artist's hand-painted detail wins)


# ─── Small node helpers (keeps the graph code readable) ──────────────────────

def _node_tree(mat):
    """Materials always have node trees from Blender 5.0; `use_nodes` is
    deprecated there but still required on 4.x."""
    if bpy.app.version < (5, 0, 0):
        mat.use_nodes = True
    return mat.node_tree


def _node(tree, kind, location=(0, 0), **props):
    n = tree.nodes.new(kind)
    n.location = location
    for key, value in props.items():
        setattr(n, key, value)
    return n


def _math(tree, op, a=None, b=None, c=None, location=(0, 0)):
    """Math node; each operand is either a socket (linked) or a float."""
    n = _node(tree, "ShaderNodeMath", location, operation=op)
    for idx, operand in enumerate((a, b, c)):
        if operand is None:
            continue
        if isinstance(operand, (int, float)):
            n.inputs[idx].default_value = float(operand)
        else:
            tree.links.new(operand, n.inputs[idx])
    return n.outputs[0]


def _smoothstep(tree, value, lo, hi, location=(0, 0)):
    n = _node(tree, "ShaderNodeMapRange", location, interpolation_type="SMOOTHSTEP")
    tree.links.new(value, n.inputs["Value"])
    n.inputs["From Min"].default_value = lo
    n.inputs["From Max"].default_value = hi
    return n.outputs["Result"]


def _voronoi(tree, coords, scale, feature, randomness, location=(0, 0)):
    n = _node(tree, "ShaderNodeTexVoronoi", location, voronoi_dimensions="3D",
              feature=feature, distance="CHEBYCHEV")
    n.inputs["Scale"].default_value = scale
    n.inputs["Randomness"].default_value = randomness
    tree.links.new(coords, n.inputs["Vector"])
    return n


def _seams(tree, coords, scale, look: Look, location=(0, 0)):
    """Rectilinear panel seam mask (1 in the groove, 0 on the plate) plus
    the F1 node, whose Color output is a per-panel random value.
    Chebyshev F2-F1 gives boxy cells; plain distance-to-edge can't do that."""
    x, y = location
    f1 = _voronoi(tree, coords, scale, "F1", look.panel_randomness, (x, y))
    f2 = _voronoi(tree, coords, scale, "F2", look.panel_randomness, (x, y - 250))
    edge = _math(tree, "SUBTRACT", f2.outputs["Distance"], f1.outputs["Distance"], location=(x + 200, y))
    flat = _smoothstep(tree, edge, 0.0, look.groove_width, (x + 380, y))
    return _math(tree, "SUBTRACT", 1.0, flat, location=(x + 560, y)), f1


# ─── Shared detail node group ────────────────────────────────────────────────

def build_detail_group(look: Look, ship_length: float):
    """Inputs: Base Color, Base Height, Detail (0..1), Shine.
    Outputs: Color, Height, Spec. One group → one place to tune the look."""
    ng = bpy.data.node_groups.new("ReskinDetail", "ShaderNodeTree")
    for name, socket in (("Base Color", "NodeSocketColor"), ("Base Height", "NodeSocketFloat"),
                         ("Detail", "NodeSocketFloat"), ("Shine", "NodeSocketFloat")):
        ng.interface.new_socket(name, in_out="INPUT", socket_type=socket)
    for name, socket in (("Color", "NodeSocketColor"), ("Height", "NodeSocketFloat"),
                         ("Spec", "NodeSocketFloat")):
        ng.interface.new_socket(name, in_out="OUTPUT", socket_type=socket)
    t = ng
    gin  = _node(t, "NodeGroupInput",  (-1600, 0))
    gout = _node(t, "NodeGroupOutput", (1400, 0))

    # Object-space coords normalised by ship length → look is size-independent.
    texco = _node(t, "ShaderNodeTexCoord", (-1600, 400))
    norm = _node(t, "ShaderNodeVectorMath", (-1400, 400), operation="SCALE")
    t.links.new(texco.outputs["Object"], norm.inputs[0])
    norm.inputs["Scale"].default_value = 1.0 / ship_length
    coords = norm.outputs[0]

    coarse, coarse_f1 = _seams(t, coords, look.panels_per_length, look, (-1200, 800))
    fine, _ = _seams(t, coords, look.panels_per_length * look.fine_panel_ratio, look, (-1200, 250))
    groove = _math(t, "MAXIMUM", coarse, _math(t, "MULTIPLY", fine, look.fine_weight,
                                                location=(-500, 250)), location=(-300, 600))

    # Per-panel jitter in [-j, +j] from the coarse cell's random colour.
    cell_rand = _node(t, "ShaderNodeSeparateColor", (-500, 900))
    t.links.new(coarse_f1.outputs["Color"], cell_rand.inputs[0])
    jitter = _math(t, "MULTIPLY", _math(t, "SUBTRACT", cell_rand.outputs[0], 0.5, location=(-300, 900)),
                   2.0 * look.panel_jitter, location=(-100, 900))

    noise = _node(t, "ShaderNodeTexNoise", (-1200, -300))
    noise.inputs["Scale"].default_value = look.grime_scale
    noise.inputs["Detail"].default_value = 8.0
    t.links.new(coords, noise.inputs["Vector"])
    grime = _smoothstep(t, noise.outputs["Fac"], 0.48, 0.72, (-900, -300))

    ao = _node(t, "ShaderNodeAmbientOcclusion", (-1200, -650), samples=16, only_local=True)
    ao.inputs["Distance"].default_value = look.ao_distance * ship_length
    ao_mul = _math(t, "MULTIPLY_ADD", ao.outputs["AO"], 1.0 - look.ao_floor, look.ao_floor,
                   location=(-900, -650))

    # Colour factor = seams × jitter × grime × AO, faded toward 1 by (1 - Detail).
    f = _math(t, "MULTIPLY_ADD", groove, -look.groove_darken, 1.0, location=(100, 500))
    f = _math(t, "MULTIPLY", f, _math(t, "ADD", jitter, 1.0, location=(100, 900)), location=(300, 500))
    f = _math(t, "MULTIPLY", f, _math(t, "MULTIPLY_ADD", grime, -look.grime_darken, 1.0,
                                      location=(100, -300)), location=(500, 500))
    f = _math(t, "MULTIPLY", f, ao_mul, location=(700, 500))
    f = _math(t, "MULTIPLY_ADD", _math(t, "SUBTRACT", f, 1.0, location=(850, 500)),
              gin.outputs["Detail"], 1.0, location=(1000, 500))
    color = _node(t, "ShaderNodeVectorMath", (1200, 400), operation="SCALE")
    t.links.new(gin.outputs["Base Color"], color.inputs[0])
    t.links.new(f, color.inputs["Scale"])
    t.links.new(color.outputs[0], gout.inputs["Color"])

    # Height: surviving source bump minus seams (fine grain from the grime noise).
    detail_h = _math(t, "MULTIPLY_ADD", noise.outputs["Fac"], 0.15, _math(t, "MULTIPLY", groove, -1.0,
                     location=(300, 100)), location=(500, 100))
    height = _math(t, "MULTIPLY_ADD", detail_h, gin.outputs["Detail"],
                   _math(t, "MULTIPLY", gin.outputs["Base Height"], look.orig_bump_weight,
                         location=(500, -50)), location=(800, 100))
    t.links.new(height, gout.inputs["Height"])

    # Spec: grime and seams are matte; panels vary a little.
    s = _math(t, "MULTIPLY_ADD", grime, -0.7, 1.0, location=(300, -100))
    s = _math(t, "MULTIPLY", s, _math(t, "MULTIPLY_ADD", groove, -0.6, 1.0, location=(300, -250)),
              location=(500, -150))
    s = _math(t, "MULTIPLY", s, _math(t, "MULTIPLY_ADD", jitter, 2.0, 1.0, location=(500, -300)),
              location=(700, -150))
    s = _math(t, "MULTIPLY_ADD", _math(t, "SUBTRACT", s, 1.0, location=(850, -150)),
              gin.outputs["Detail"], 1.0, location=(1000, -150))
    spec = _math(t, "MULTIPLY", s, gin.outputs["Shine"], location=(1200, -150))
    t.links.new(spec, gout.inputs["Spec"])
    return ng


# ─── Scene prep ──────────────────────────────────────────────────────────────

def enable_gpu(scene) -> str:
    """Use a GPU Cycles device when available, else CPU. CUDA comes before
    OptiX on purpose: OptiX's RT-core speedup is irrelevant for bakes and
    its PTX JIT breaks on some driver/Blender combos (seen on 5.2 + a
    3090: `optix.ptx.copysign.f32` unimplemented) — silently."""
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for backend in ("CUDA", "HIP", "METAL", "ONEAPI", "OPTIX"):
        try:
            prefs.compute_device_type = backend
        except TypeError:
            continue
        prefs.get_devices()
        gpus = [d for d in prefs.devices if d.type == backend]
        if gpus:
            for d in gpus:
                d.use = True
            scene.cycles.device = "GPU"
            return backend
    scene.cycles.device = "CPU"
    return "CPU"


def import_joined(ship: str):
    """Import the OBJ and join every part into one object named after the ship."""
    bpy.ops.wm.obj_import(filepath=str(SRC_DIR / f"{ship}.obj"))
    parts = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    if len(parts) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = ship
    uvs = obj.data.uv_layers
    if not uvs:
        uvs.new(name=ORIG_UV)
    uvs[0].name = ORIG_UV
    return obj


def unwrap_atlas(obj, margin: float) -> None:
    atlas = obj.data.uv_layers.new(name=ATLAS_UV)
    obj.data.uv_layers.active = atlas
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=margin)
    bpy.ops.uv.pack_islands(rotate=True, margin=margin)
    bpy.ops.object.mode_set(mode="OBJECT")


def ship_length(obj) -> float:
    return max(obj.dimensions)


def _image_node(tree, filename: str, non_color: bool, location):
    """Image texture sampled through the ORIGINAL UVs."""
    img = bpy.data.images.load(str(SRC_DIR / filename), check_existing=True)
    if non_color:
        img.colorspace_settings.name = "Non-Color"
    tex = _node(tree, "ShaderNodeTexImage", location, image=img)
    uv = _node(tree, "ShaderNodeUVMap", (location[0] - 200, location[1]), uv_map=ORIG_UV)
    tree.links.new(uv.outputs["UV"], tex.inputs["Vector"])
    return tex


def _detail_strength(name: str, spec: dict, look: Look) -> float:
    """Full procedural detail on flat tints, a light touch over authored
    bitmaps, none on glass/lights."""
    if any(k in name for k in NO_DETAIL_KEYWORDS):
        return 0.0
    is_flat_tint = "_tint_" in spec.get("diffuse", "_tint_")
    return 1.0 if is_flat_tint else look.authored_detail


def wire_material(mat, spec: dict, group, look: Look, length: float) -> None:
    """Rebuild one source material as: source maps → detail group → bake
    nodes. The `bake_target` image node is left selected+active; bake()
    points it at each pass's output image."""
    name = spec["name"].upper()
    t = _node_tree(mat)
    t.nodes.clear()
    detail = _node(t, "ShaderNodeGroup", (0, 0), node_tree=group)
    detail.inputs["Detail"].default_value = _detail_strength(name, spec, look)
    detail.inputs["Shine"].default_value = (look.shiny_spec if any(k in name for k in SHINY_KEYWORDS)
                                            else look.base_spec)
    if spec.get("diffuse"):
        diffuse = _image_node(t, spec["diffuse"], False, (-400, 200))
        t.links.new(diffuse.outputs["Color"], detail.inputs["Base Color"])
    # NB: the wcnews "normal" key actually holds a grayscale 3DS bump map (#473).
    if spec.get("normal"):
        bump_src = _image_node(t, spec["normal"], True, (-400, -150))
        t.links.new(bump_src.outputs["Color"], detail.inputs["Base Height"])

    bump = _node(t, "ShaderNodeBump", (250, -200))
    bump.inputs["Distance"].default_value = look.bump_distance * length
    t.links.new(detail.outputs["Height"], bump.inputs["Height"])
    bsdf = _node(t, "ShaderNodeBsdfPrincipled", (450, -100), name="bake_bsdf")
    t.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    _node(t, "ShaderNodeEmission", (450, 150), name="bake_emit")
    _node(t, "ShaderNodeOutputMaterial", (700, 0), name="bake_out")

    tex = _node(t, "ShaderNodeTexImage", (450, 400), name="bake_target")
    atlas_uv = _node(t, "ShaderNodeUVMap", (250, 400), uv_map=ATLAS_UV)
    t.links.new(atlas_uv.outputs["UV"], tex.inputs["Vector"])
    for n in t.nodes:
        n.select = False
    tex.select = True
    t.nodes.active = tex


# ─── Baking ──────────────────────────────────────────────────────────────────

def _route(mat, source_socket_name: str | None) -> None:
    """Point the material output at either the BSDF (normal bake) or an
    emission shader carrying one of the detail group's outputs."""
    t = mat.node_tree
    out, emit, bsdf = t.nodes["bake_out"], t.nodes["bake_emit"], t.nodes["bake_bsdf"]
    for link in list(out.inputs["Surface"].links):
        t.links.remove(link)
    if source_socket_name is None:
        t.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
        return
    for link in list(emit.inputs["Color"].links):
        t.links.remove(link)
    group = next(n for n in t.nodes if n.type == "GROUP")
    t.links.new(group.outputs[source_socket_name], emit.inputs["Color"])
    t.links.new(emit.outputs["Emission"], out.inputs["Surface"])


def bake(materials, image, kind: str, source: str | None, margin_px: int) -> None:
    for mat in materials:
        mat.node_tree.nodes["bake_target"].image = image
        _route(mat, source)
    extra = {}
    if kind == "NORMAL":
        # mesh.glsl flips V (uv.y = 1 - v) before building its cotangent
        # frame, so its bitangent points toward -V. Blender's +Y tangent
        # space points toward +V → flip green (DirectX-style) to match.
        extra = dict(normal_space="TANGENT", normal_r="POS_X", normal_g="NEG_Y", normal_b="POS_Z")
    bpy.ops.object.bake(type=kind, margin=margin_px, use_clear=True, **extra)
    _assert_baked(image)


def _assert_baked(image) -> None:
    """Cycles reports kernel/device failures to the log but the bake
    operator still returns FINISHED, leaving the fill colour behind."""
    px = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(px)
    spread = float(px.reshape(-1, 4)[:, :3].std(axis=0).max())
    if spread < 1e-3:
        raise SystemExit(f"[reskin] bake of '{image.name}' produced a flat image "
                         f"(std {spread:.2e}) — check the Cycles log above")


def new_image(name: str, size: int, non_color: bool, fill):
    img = bpy.data.images.new(name, size, size, alpha=False)
    img.generated_color = fill
    if non_color:
        img.colorspace_settings.name = "Non-Color"
    return img


# ─── Export ──────────────────────────────────────────────────────────────────

def export(obj, ship: str, images: dict) -> Path:
    DST_DIR.mkdir(parents=True, exist_ok=True)
    files = {}
    for slot, img in images.items():
        fname = f"{ship}_{slot}.png"
        img.filepath_raw = str(DST_DIR / fname)
        img.file_format = "PNG"
        img.save()
        files[slot] = fname

    # One material, one UV set: exactly what the engine will read. The
    # node graph only feeds the companion .mtl (handy for DCC previews).
    final = bpy.data.materials.new(FINAL_MAT)
    t = _node_tree(final)
    t.nodes.clear()
    bsdf = _node(t, "ShaderNodeBsdfPrincipled", (0, 0))
    base = _node(t, "ShaderNodeTexImage", (-300, 0), image=images["diffuse"])
    t.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    t.links.new(bsdf.outputs["BSDF"], _node(t, "ShaderNodeOutputMaterial", (300, 0)).inputs["Surface"])
    obj.data.materials.clear()
    obj.data.materials.append(final)
    obj.data.uv_layers.remove(obj.data.uv_layers[ORIG_UV])

    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    out = DST_DIR / f"{ship}.obj"
    bpy.ops.wm.obj_export(filepath=str(out), export_selected_objects=True, export_uv=True,
                          export_normals=True, export_materials=True,
                          export_triangulated_mesh=True, path_mode="STRIP")
    sidecar = {"materials": [{"name": FINAL_MAT, **files}]}
    (DST_DIR / f"{ship}.materials.json").write_text(json.dumps(sidecar, indent=2) + "\n")
    return out


def _vertex_bbox(obj_path: Path) -> tuple[list[float], list[float]]:
    lo, hi = [math.inf] * 3, [-math.inf] * 3
    with obj_path.open() as f:
        for line in f:
            if line.startswith("v "):
                for i, v in enumerate(map(float, line.split()[1:4])):
                    lo[i], hi[i] = min(lo[i], v), max(hi[i], v)
    return lo, hi


def assert_same_frame(src: Path, dst: Path) -> None:
    """The engine's per-ship euler overrides and lights3d files assume the
    wcnews frame. Fail loudly if the round trip moved/rotated/scaled it."""
    (slo, shi), (dlo, dhi) = _vertex_bbox(src), _vertex_bbox(dst)
    tol = 1e-3 * max(b - a for a, b in zip(slo, shi))
    drift = max(abs(a - b) for a, b in zip(slo + shi, dlo + dhi))
    if drift > tol:
        raise SystemExit(f"[reskin] frame drift {drift:.4f} > {tol:.4f}: "
                         f"src {slo}..{shi} vs out {dlo}..{dhi}")


# ─── Main ────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--ship", required=True, help="stem under ships_wcnews/, e.g. dd_tug")
    # 2048 already gives ~2x the texel density ships ever get on screen
    # (≤512px sprite cells), and material.cpp uploads a single mip level,
    # so bigger atlases only add VRAM and minification shimmer.
    p.add_argument("--size", type=int, default=2048, help="atlas resolution (square)")
    p.add_argument("--samples", type=int, default=64, help="Cycles samples per bake")
    p.add_argument("--margin", type=float, default=0.003, help="UV island margin")
    return p.parse_args(argv)


def main() -> None:
    args = parse_args()
    look = Look()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    device = enable_gpu(scene)
    scene.cycles.samples = args.samples

    obj = import_joined(args.ship)
    length = ship_length(obj)
    unwrap_atlas(obj, args.margin)

    specs = {s["name"]: s for s in json.loads(
        (SRC_DIR / f"{args.ship}.materials.json").read_text())["materials"]}
    group = build_detail_group(look, length)
    images = {
        "diffuse": new_image(f"{args.ship}_diffuse", args.size, False, (0.5, 0.5, 0.5, 1)),
        "normal":  new_image(f"{args.ship}_normal",  args.size, True,  (0.5, 0.5, 1.0, 1)),
        "spec":    new_image(f"{args.ship}_spec",    args.size, True,  (0.5, 0.5, 0.5, 1)),
    }
    materials = [m for m in obj.data.materials if m]
    for mat in materials:
        wire_material(mat, specs.get(mat.name, {"name": mat.name}), group, look, length)

    margin_px = max(4, args.size // 256)
    bake(materials, images["diffuse"], "EMIT", "Color", margin_px)
    bake(materials, images["spec"], "EMIT", "Spec", margin_px)
    bake(materials, images["normal"], "NORMAL", None, margin_px)

    out = export(obj, args.ship, images)
    assert_same_frame(SRC_DIR / f"{args.ship}.obj", out)
    print(f"[reskin] {args.ship}: {len(materials)} source materials, {args.size}px atlas, "
          f"device={device}, length={length:.1f} -> {out}")


if __name__ == "__main__":
    main()
