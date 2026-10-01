"""The atrium walkers' routes (#605), in world metres. Pure stdlib, so
render_layers.py (Blender) walks them and check_camera.py (uv) draws them.

Routes are waypoints round the kiosk's axis, (radius m, degrees) as in
camera_match.around_axis(), on one floor; between waypoints radius and angle
change together, so a constant angle is a straight radial walk and a constant
radius an arc. Measured on the plate (camera_match.floor_point()):

* the bridge's centreline (770, 1015) - (950, 950) and the arm's (1300, 858) -
  (1500, 810) are both radial, at -148 and 24 deg,
* on the lower ring the walkway kerb is r 14.6, the tiled floor outside it
  runs to r 20.5 and the glass lobby's front stands at r ~25.5.
"""
import math

import camera_match as cm

BRIDGE_DEG = -148.0
ARM_DEG = 24.0
LOBBY_R = 25.5              # the glass lobby's front, on the lower ring
TILES_EDGE_R = 20.5         # the outer edge of the lower ring's tiled floor
TILES_R = 17.6              # its middle


def polyline(waypoints, step_m=0.5):
    """World (x, y) points along polar `waypoints`, about every `step_m`."""
    points = []
    for (r0, d0), (r1, d1) in zip(waypoints, waypoints[1:]):
        x0, y0 = cm.around_axis(r0, d0)
        x1, y1 = cm.around_axis(r1, d1)
        arc = math.radians(abs(d1 - d0)) * max(r0, r1)
        n = max(1, round(max(arc, math.dist((x0, y0), (x1, y1))) / step_m))
        points += [cm.around_axis(r0 + (r1 - r0) * k / n, d0 + (d1 - d0) * k / n)
                   for k in range(n)]
    points.append(cm.around_axis(*waypoints[-1]))
    return points


ROUTES = {
    # Up the bridge from below the frame, onto the kiosk's platform and round
    # the near side of its column (under the canopy's rim, 2.6 m up), then
    # out along the arm and off frame-right. Out of shot he curves in from
    # under the camera: straight up the radial line, his head would rise into
    # frame over the planters left of the bridge before his feet reached it.
    "walker_bridge": {
        "z": cm.DECK_Z, "speed": 1.3, "model": "mechanic_chair_sit_idle_m.glb",
        "waypoints": [(9.5, -125.0), (6.0, BRIDGE_DEG), (2.9, BRIDGE_DEG), (2.0, -122.0),
                      (2.0, -4.0), (2.9, ARM_DEG), (13.5, ARM_DEG)],
    },
    # Out of the glass lobby behind the kiosk: from behind its wall (inside,
    # seen through the glass), out through the doors, then down the tiled
    # floor round the ring to the bottom left, under the cantilevered beam's
    # end and behind the balcony's lip.
    "walker_lobby": {
        "z": cm.LOWER_Z, "speed": 1.2, "model": "rustbound_ranger_sit_cross_legged.glb",
        "waypoints": [(27.5, 90.0), (27.5, 106.0), (LOBBY_R - 1.0, 108.0),
                      (TILES_R + 1.0, 114.0), (TILES_R, 122.0), (TILES_R, 175.0)],
    },
}


def path(name):
    """A route's world (x, y) polyline."""
    return polyline(ROUTES[name]["waypoints"])
