"""Procedural actors for the New Con concourse layers (#515).

Deliberately low-detail proxies: the largest a hover-car ever gets on screen
is ~250 px wide and it spends most of its pass far smaller, so silhouette,
lights and floor interaction sell it, not modelling detail.
"""
import bpy

from scene import AMBER, material
from stage import animate_straight_pass  # noqa: F401  (shared; used as actors.*)
from walkers import animate_walk, build_walker  # noqa: F401  (shared; used as actors.*)
from walkers import shaped_box as _box


def build_hover_car(name="HoverCar", hull_rgb=(0.42, 0.44, 0.47)):
    """A ~4.6 m hover-car facing +Y, origin at its hover centre."""
    root = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(root)

    hull = material(f"{name}Hull", hull_rgb, metallic=0.85, roughness=0.32)
    glass = material(f"{name}Glass", (0.02, 0.03, 0.05), metallic=0.0, roughness=0.05)
    trim = material(f"{name}Trim", (0.06, 0.06, 0.07), metallic=0.9, roughness=0.45)
    head = material(f"{name}Head", (1.0, 0.9, 0.7), emission=(1.0, 0.85, 0.6), strength=20.0)
    tail = material(f"{name}Tail", (0.8, 0.05, 0.03), emission=(1.0, 0.06, 0.02), strength=6.0)
    glow = material(f"{name}Glow", AMBER, emission=AMBER, strength=4.0)

    _box("Hull", (2.0, 4.6, 0.6), (0, 0, 0), hull, bevel=0.22, parent=root,
         nose_taper=0.72, nose_drop=0.45)
    _box("Skirt", (2.1, 4.3, 0.16), (0, 0, -0.34), trim, bevel=0.06, parent=root,
         nose_taper=0.75)
    _box("Canopy", (1.4, 1.9, 0.48), (0, -0.4, 0.46), glass, bevel=0.2, parent=root,
         nose_taper=0.6, nose_drop=0.55)
    _box("Fin", (1.9, 0.35, 0.08), (0, -2.1, 0.62), trim, bevel=0.03, parent=root)
    _box("HeadL", (0.4, 0.06, 0.08), (-0.5, 2.3, -0.05), head, parent=root)
    _box("HeadR", (0.4, 0.06, 0.08), (0.5, 2.3, -0.05), head, parent=root)
    _box("TailStrip", (1.6, 0.06, 0.07), (0, -2.31, 0.1), tail, parent=root)
    _box("Underglow", (1.4, 3.2, 0.04), (0, 0, -0.44), glow, parent=root)
    # Warm light pooling on the deck under the car: this is what reads as
    # "hovering" in the reflection, even when the car is a few pixels tall.
    pool = bpy.data.lights.new(f"{name}Pool", 'AREA')
    pool.color, pool.energy, pool.size = AMBER, 70.0, 2.5
    pool_obj = bpy.data.objects.new(f"{name}Pool", pool)
    pool_obj.location, pool_obj.parent = (0, 0, -0.5), root
    bpy.context.scene.collection.objects.link(pool_obj)
    return root
