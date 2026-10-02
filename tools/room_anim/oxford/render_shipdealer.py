"""Render the raw passes for the Oxford ship dealer's salesman (#682).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/oxford/render_shipdealer.py -- --check
    ... -- --layer salesman
    ... -- --layer salesman --frames 1:24        # quick look

The repainted ship dealer (#675, assets/concourse/oxford/shipdealer_bg.png)
looks steeply down on a wood-decked showroom with three ships on it, and a
salesman standing on the walkway between the hazard lanes, presenting the
needle-nosed fighter. Here he walks the floor instead and presents each ship
in turn: the fighter, the cockpit ship on the right, the heavy fighter at
the bottom, then back to his painted spot (walkers.plan_tour).

The camera (pitched_camera.py) is a long lens looking 50 deg down: the
hazard lanes' rails barely converge (parallel to a pixel over 440 px), and
the lamp posts at either side lean out from a nadir ~4500 px below the
centre. Scale: the painted salesman, head top y 489 over his feet at
(781, 570), is a 1.75 m man, which puts the eye 54 m up (72 px/m across
the deck at his spot).

--check renders him at the three ships, on the deck as a shadow catcher,
over the plate: build/room_anim/oxford/shipdealer/check.png.

Passes go to build/room_anim/oxford/shipdealer/<layer>/ (see ../render.py);
`bake_layer.py --base oxford --room shipdealer` turns them into sprites,
encoded over the plate with the painted salesman painted out
(bake_shipdealer_patch.py, the layer's "over").
"""
import argparse
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402
import stage  # noqa: E402
import walkers  # noqa: E402
from base import paths  # noqa: E402
from pitched_camera import PitchedCamera  # noqa: E402

SHIPDEALER = paths("oxford", "shipdealer")
CAM = PitchedCamera(focal_px=5000.0, pitch_deg=50.0, eye=54.0)
FPS = 12                    # as New Detroit's walkers: ample for an 81 px figure
MODEL = "weathered_sentinel_chair_sit_idle_m.glb"   # dark hair, olive jacket
SPEED = 1.0                 # m/s: a showroom stroll

# Landmarks, plate px. His feet on the painted spot, and where he stops by
# the other two ships, clear of the lanes' rails. Each ship is beside him
# on screen, not below him: an arm aimed down the screen (toward the lens)
# only crossed his own body, so he presents the heavy fighter from the deck
# at its right, between its hull and the right lane.
SPOT = (781.0, 570.0)
BY_COCKPIT_SHIP = (915.0, 615.0)
BY_HEAVY_FIGHTER = (820.0, 760.0)
# What he points at: a point on each hull (plate px) and its height (m).
FIGHTER = ((430.0, 420.0), 1.6)
COCKPIT_SHIP = ((1118.0, 698.0), 1.4)
HEAVY_FIGHTER = ((640.0, 790.0), 2.0)
HOLD_S = 2.5                # each ship's pitch
# He presents a ship side-on: it's this far round to one side of him, the
# side that leaves him most open to the viewer (the customer). Pointing
# straight at a ship below him aimed the arm into the lens: invisible.
SHIP_SIDE_DEG = 65.0
# The painted salesman's look from the model's texture: sage-green tweed
# from its olive jacket, near-black trousers, hair and shoes. Each rule
# scales the base colour's texels in a hue range (0-1) and saturation range
# by a gain; skin (hue 0.05-0.10, saturation ~0.3) is in none of them.
# Ranges are open (lo < v < hi): a pure grey has hue and saturation 0.
RECOLOUR = [
    ((0.10, 0.45), (0.08, 0.30), (1.2, 1.45, 1.2)),     # the olive jacket -> tweed
    ((0.50, 0.80), (0.08, 1.01), (0.5, 0.5, 0.55)),     # blue-grey trousers -> charcoal
    ((-1.0, 2.0), (-1.0, 0.08), (0.55, 0.55, 0.55)),    # greys: hair, shoes, belt
]

WOOD = (0.11, 0.055, 0.025)
KEY_RGB, KEY_W = (1.0, 0.82, 0.62), 4.0
# The painting disagrees with itself: the salesman's shadow falls down the
# screen (light from behind), but his face and jacket are lit from the
# front. A key from behind alone rim-lit his whole outline and left his
# front dark, so: a steep key, a touch behind and right (his shadow still
# falls down the screen and a little left, shorter), and a shadowless fill
# from the camera for his front.
KEY_TRAVEL = Vector((-0.1, -0.35, -1.0))
FILL_RGB, FILL_W = (1.0, 0.92, 0.8), 1.5
AMBIENT = (0.09, 0.075, 0.065)


def floor(px):
    """Plate px -> world (x, y) on the deck."""
    return CAM.ground_point(*px)


def hull(landmark):
    (px, py), z = landmark
    return (*CAM.ground_point(px, py, z), z)


def _off(a, b):
    return abs((a - b + math.pi) % (2.0 * math.pi) - math.pi)


def _present(at, ship):
    """A present beat at floor point `at`: the ship SHIP_SIDE_DEG to his side."""
    target = hull(ship)
    to_ship = walkers.heading_to(*at, *target[:2])
    to_viewer = walkers.heading_to(*at, 0.0, 0.0)          # the camera's foot
    side = math.radians(SHIP_SIDE_DEG)
    face = min((to_ship - side, to_ship + side), key=lambda h: _off(h, to_viewer))
    return {"present": target, "hold": HOLD_S, "face": face}


def tour():
    """The loop's states (walkers.plan_tour). It starts standing on the spot,
    as the last walk leaves him, so it's planned twice: the second time from
    the heading the first one ended on."""
    spot, cockpit, heavy = floor(SPOT), floor(BY_COCKPIT_SHIP), floor(BY_HEAVY_FIGHTER)
    beats = [_present(spot, FIGHTER), {"walk": [cockpit]},
             _present(cockpit, COCKPIT_SHIP), {"walk": [heavy]},
             _present(heavy, HEAVY_FIGHTER), {"walk": [spot]}]
    heading = walkers.heading_to(*heavy, *spot)
    for _ in range(2):
        start, states = heading, walkers.plan_tour(spot, heading, beats, FPS, SPEED)
        heading = states[-1].heading
    # The last frame is the pose the first one steps on from.
    drift = abs((states[-1].heading - start + math.pi) % (2.0 * math.pi) - math.pi)
    assert drift < math.radians(1.0), f"the loop doesn't close: {math.degrees(drift):.1f} deg"
    return states


def add_camera(sc):
    data = bpy.data.cameras.new("ShipDealerCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = CAM.lens_mm
    data.clip_start, data.clip_end = 1.0, 1000.0
    cam = bpy.data.objects.new("ShipDealerCam", data)
    sc.collection.objects.link(cam)
    # Level and facing +Y is X 90 deg; pitched down from there.
    cam.matrix_world = (Matrix.Translation((0.0, 0.0, CAM.eye)) @
                        Matrix.Rotation(math.radians(90.0 - CAM.pitch_deg), 4, 'X'))
    sc.camera = cam
    stage.attach_plate_reference(cam, str(SHIPDEALER.plate))
    return cam


def add_lights(sc):
    world = bpy.data.worlds.new("ShipDealerAmbient")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*AMBIENT, 1.0)
    sc.world = world
    key = stage.light(sc, "Key", 'SUN', (0.0, 0.0, 0.0), KEY_RGB, KEY_W)
    key.rotation_euler = KEY_TRAVEL.to_track_quat('-Z', 'Y').to_euler()
    key.data.angle = math.radians(4.0)        # soft-edged, like the painted shadow
    fill = stage.light(sc, "Fill", 'SUN', (0.0, 0.0, 0.0), FILL_RGB, FILL_W)
    view = Vector((0.0, math.cos(math.radians(CAM.pitch_deg)), -math.sin(math.radians(CAM.pitch_deg))))
    fill.rotation_euler = view.to_track_quat('-Z', 'Y').to_euler()
    fill.data.use_shadow = False


def build_scene(samples=48):
    sc = stage.reset()
    stage.setup_render(sc, samples)
    sc.render.fps = FPS
    add_camera(sc)
    x, y = floor((768.0, 512.0))
    deck = stage.box_object("Deck", (60.0, 80.0, 0.02), (x, y, -0.01),
                            stage.material("Wood", WOOD, roughness=0.35), sc.collection)
    add_lights(sc)
    return sc, [deck]


def recolour(root, rules):
    """Apply `rules` (RECOLOUR) to every material under `root`: each scales
    the base colour's texels inside its hue and saturation ranges (masks on
    the original colour, so rules don't compound), chained in the shader."""
    for mat in {s.material for o in root.children_recursive if o.type == 'MESH'
                for s in o.material_slots if s.material}:
        nt = mat.node_tree
        bsdf = next(n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED')
        base = colour = bsdf.inputs["Base Color"].links[0].from_socket
        hsv = nt.nodes.new("ShaderNodeSeparateColor")
        hsv.mode = 'HSV'
        nt.links.new(base, hsv.inputs[0])

        def op(kind, a, b):
            node = nt.nodes.new("ShaderNodeMath")
            node.operation = kind
            for socket, v in zip(node.inputs, (a, b)):
                if isinstance(v, float):
                    socket.default_value = v
                else:
                    nt.links.new(v, socket)
            return node.outputs[0]

        def within(value, lo, hi):
            return op('MULTIPLY', op('GREATER_THAN', value, lo), op('LESS_THAN', value, hi))

        h, s = hsv.outputs[0], hsv.outputs[1]
        for hues, sats, gain in rules:
            mix = nt.nodes.new("ShaderNodeMix")
            mix.data_type, mix.blend_type = 'RGBA', 'MULTIPLY'
            fac, a, b = (sock for sock in mix.inputs if sock.enabled)
            nt.links.new(op('MULTIPLY', within(h, *hues), within(s, *sats)), fac)
            nt.links.new(colour, a)
            b.default_value = (*gain, 1.0)
            colour = next(o for o in mix.outputs if o.enabled)
        nt.links.new(colour, bsdf.inputs["Base Color"])


def add_salesman(states=None):
    root, gait = walkers.build_rigged_walker("Salesman", MODEL)
    recolour(root, RECOLOUR)
    last = walkers.key_tour(root, gait, states or tour())
    return root, last


def build(layer):
    sc, decks = build_scene()
    root, last = LAYERS[layer]()
    sc.frame_start, sc.frame_end = 1, last
    return sc, decks, [root]


# name -> builder() -> (actor root, last frame)
LAYERS = {"salesman": add_salesman}


def check(out_png):
    """Him at each ship mid-pitch, his shadow on the deck, over the plate."""
    sc, (deck,) = build_scene(samples=16)
    sc.view_settings.exposure = 0.0            # as baked: the bake undoes EXPOSURE_EV
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    deck.is_shadow_catcher = True
    states = tour()
    add_salesman(states)
    holds = [f for f, (a, b) in enumerate(zip(states, states[1:]), start=1)
             if a.weight < 1.0 <= b.weight]   # the first frame of each hold
    plate = SHIPDEALER.plate
    for i, f in enumerate(holds):
        sc.frame_set(f + 1)
        guides = out_png.with_name(f"check_{i}.png")
        sc.render.filepath = str(guides)
        bpy.ops.render.render(write_still=True)
        stage.overlay_on_plate(plate, guides, out_png)
        plate = out_png
    print(f"[render_shipdealer] {len(states)} frames ({len(states) / FPS:.1f} s); "
          f"check frames {[f + 1 for f in holds]}", flush=True)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="him at each ship, over the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    SHIPDEALER.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(SHIPDEALER.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, SHIPDEALER.build / layer, frames)
        print(f"[render_shipdealer] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
