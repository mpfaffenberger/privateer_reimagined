"""The Agricultural concourse atrium's camera, as pure math (#605). Stdlib
only, so it runs both inside Blender (scene.py builds the camera from it) and
under uv (check_camera.py draws the fitted rings and routes over the plate).

The painting (assets/concourse/agricultural/concourse_bg.png) looks down from
a high balcony into a round atrium: a kiosk on a round platform at the centre,
reached by a bridge from the near side with an arm off to the right, and a
lower ring floor below it that curves from the bottom left round to a glass
doorway behind the kiosk. Nothing in it is straight except the bridge, so the
fit is to circles about the kiosk's axis:

* the verticals are vertical (the kiosk's pole and column, the doorway's
  jambs), so the camera is level with lens shift, and the axis (the pole,
  x 1090) is straight ahead: the upper ring's crest sits over it,
* the horizon is at y 175, the far ridges through the windows (the window
  traffic's own level camera put them at y 172, #582),
* f 1380 px and the axis 26.4 m out (1.89 eye heights) from a least-squares
  fit of three circles round the axis: the platform's rim (deck level), the
  kiosk's canopy and the lower ring's walkway kerb (~3 px residual; the
  canopy, ~8 px, is painted a little rounder than the rest),
* scale from the kiosk: an eye 14 m above the deck makes the platform 5.7 m
  across, the kiosk's column 1.6 m and its canopy 2.6 m up, a door's height;
  the lower ring is then 5.6 m below the deck.

World: metres, camera at (0, 0, EYE_HEIGHT) looking down +Y (level, so view
depth is y), +X right, Z up, the bridge deck at Z = 0. The kiosk's axis is at
(0, AXIS_Y).
"""
import math

PLATE_W, PLATE_H = 1536, 1024
AXIS_X_PX = 1090.0          # the kiosk's pole: the principal point's x (lens shift)
HORIZON_Y = 175.0           # the principal point's y (lens shift)
FOCAL_PX = 1380.0
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W
EYE_HEIGHT = 14.0           # m above the bridge deck

AXIS_Y = 26.4               # m, the kiosk's axis
DECK_Z = 0.0                # the bridge, its arm and the kiosk's platform
LOWER_Z = -5.63             # the lower ring floor
PLATFORM_R = 2.86           # the kiosk platform's rim
COLUMN_R = 0.8              # the kiosk's column (x 1050-1130 at the platform)
KERB_R = 14.6               # the lower ring's kerb, tiles outside, walkway inside


def project(x, y, z=0.0):
    """Plate pixel (px, py) of world point (x, y, z)."""
    return (AXIS_X_PX + FOCAL_PX * x / y, HORIZON_Y + FOCAL_PX * (EYE_HEIGHT - z) / y)


def floor_point(px, py, z=0.0):
    """World (x, y) of the surface at height `z` seen at plate pixel (px, py)."""
    y = FOCAL_PX * (EYE_HEIGHT - z) / (py - HORIZON_Y)
    return ((px - AXIS_X_PX) * y / FOCAL_PX, y)


def on_upright(px, py, y):
    """World (x, y, z) on plate pixel (px, py)'s ray at view depth `y`: a
    point of an upright card square to the camera there."""
    return ((px - AXIS_X_PX) * y / FOCAL_PX, y, EYE_HEIGHT - (py - HORIZON_Y) * y / FOCAL_PX)


def around_axis(radius, deg):
    """World (x, y) on a circle round the kiosk's axis; 0 deg = +X (frame
    right), 90 = away from the camera, -90 = toward it."""
    a = math.radians(deg)
    return (radius * math.cos(a), AXIS_Y + radius * math.sin(a))


def axis_angle(x, y):
    """Degrees round the kiosk's axis of world (x, y); inverse of around_axis()."""
    return math.degrees(math.atan2(y - AXIS_Y, x))
