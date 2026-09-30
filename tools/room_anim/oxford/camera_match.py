"""The Oxford concourse plate's camera, as pure math (#592). Stdlib only, so
it runs both inside Blender (scene.py builds the camera from it) and under
uv (check_camera.py draws the matched street grid over the plate).

The painting (assets/concourse/oxford/concourse_bg.png) looks down on the
university's garden square from high up: a long lens, three-point
perspective, with the street grid at ~45 deg to the view. Fitted with
OpenCV's line-segment detector: street edges and roof lines fall into two
orthogonal families (image angles ~27 and ~148 deg) plus the tower's
verticals. A least-squares fit of focal length, pitch and roll (principal
point at the centre, no lens shift) to all three families' vanishing points
leaves ~0.4 deg median residual on the streets:

* f = 2895 px (a 67.9 mm lens on 36 mm), pitched 34.4 deg down, rolled
  -0.4 deg; the street grid runs at -43.9 deg to the camera's X axis,
* horizon ~y = -1440 px, far above the frame; the verticals meet ~y = 4750,
* scale from the painting's street lamps (~4 m, base y 680 -> heads y 605 at
  x 533) and the tower's archways (~4.5 m, 70 px): eye 80 m up. The central
  garden then comes out ~17 m square.

World: metres, camera at (0, 0, EYE_HEIGHT) looking down +Y, +X right, Z up,
streets at Z = 0. Street frame: `u` along the streets that run down-right on
screen, `v` along the streets that run up-right, both from STREET_ORIGIN.
"""
import math

PLATE_W, PLATE_H = 1536, 1024
FOCAL_PX = 2895.4
PITCH_DEG = 34.363          # down from level
ROLL_DEG = -0.394           # about the view axis
EYE_HEIGHT = 80.0           # m
GRID_DEG = -43.872          # the u street axis, from world +X toward +Y
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W

_P, _R = math.radians(PITCH_DEG), math.radians(ROLL_DEG)
_G = math.radians(GRID_DEG)


def ray(px, py):
    """World direction (x, y, z) through plate pixel (px, py)."""
    x, y = px - PLATE_W / 2, py - PLATE_H / 2
    cr, sr = math.cos(_R), math.sin(_R)
    x, y = cr * x + sr * y, -sr * x + cr * y                 # undo the roll
    cp, sp = math.cos(_P), math.sin(_P)
    # Level camera: right = +X, down = -Z, forward = +Y; then pitched down.
    return (x, FOCAL_PX * cp - y * sp, -FOCAL_PX * sp - y * cp)


def ground_point(px, py, z=0.0):
    """World (x, y) of the surface at height `z` under plate pixel (px, py)."""
    dx, dy, dz = ray(px, py)
    t = (z - EYE_HEIGHT) / dz
    return (dx * t, dy * t)


def project(x, y, z=0.0):
    """Plate pixel (px, py) of world point (x, y, z); inverse of ray()."""
    cp, sp = math.cos(_P), math.sin(_P)
    wz = z - EYE_HEIGHT
    depth = y * cp - wz * sp
    down = -y * sp - wz * cp
    cr, sr = math.cos(_R), math.sin(_R)
    right, down = cr * x - sr * down, sr * x + cr * down     # redo the roll
    return (PLATE_W / 2 + FOCAL_PX * right / depth, PLATE_H / 2 + FOCAL_PX * down / depth)


def depth(x, y, z=0.0):
    """Distance of world point (x, y, z) along the camera's view axis."""
    return y * math.cos(_P) + (EYE_HEIGHT - z) * math.sin(_P)


def at_depth(px, py, d):
    """World (x, y, z) on plate pixel (px, py)'s ray at view depth `d`."""
    rx, ry, rz = ray(px, py)
    t = d / FOCAL_PX                  # ray()'s view-axis component is FOCAL_PX
    return (rx * t, ry * t, EYE_HEIGHT + rz * t)


# Street-frame origin: the street corner at the central garden's west tip.
STREET_ORIGIN = ground_point(548.0, 675.0)


def to_world(u, v):
    """Street frame (u, v) metres -> world (x, y)."""
    cg, sg = math.cos(_G), math.sin(_G)
    return (STREET_ORIGIN[0] + u * cg - v * sg, STREET_ORIGIN[1] + u * sg + v * cg)


def to_street(x, y):
    """World (x, y) -> street frame (u, v)."""
    cg, sg = math.cos(_G), math.sin(_G)
    dx, dy = x - STREET_ORIGIN[0], y - STREET_ORIGIN[1]
    return (dx * cg + dy * sg, -dx * sg + dy * cg)


def heading_deg(u0, v0, u1, v1):
    """World heading (deg about Z, 0 = facing +Y) of travel from (u0, v0)
    to (u1, v1), for actors modelled nose +Y."""
    (x0, y0), (x1, y1) = to_world(u0, v0), to_world(u1, v1)
    return math.degrees(math.atan2(-(x1 - x0), y1 - y0))
