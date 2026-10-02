"""A level-then-pitched pinhole camera over a plate, as pure math (#682).
Stdlib only, so it runs both inside Blender (a scene builds its camera from
it) and under uv (plate-px landmarks -> world).

The same model as oxford/camera_match.py, which has it as module constants
(moving it onto this class: #687): principal point at the plate centre, no
lens shift, rolled about the view axis. World: metres, camera at (0, 0, eye) looking down +Y, +X right, Z up,
the floor at Z = 0.
"""
import math

PLATE_W, PLATE_H = 1536, 1024
SENSOR_MM = 36.0


class PitchedCamera:
    def __init__(self, focal_px, pitch_deg, eye, roll_deg=0.0):
        self.focal_px, self.pitch_deg, self.eye, self.roll_deg = focal_px, pitch_deg, eye, roll_deg
        self.lens_mm = focal_px * SENSOR_MM / PLATE_W
        p, r = math.radians(pitch_deg), math.radians(roll_deg)
        self._cp, self._sp, self._cr, self._sr = math.cos(p), math.sin(p), math.cos(r), math.sin(r)

    def ray(self, px, py):
        """World direction (x, y, z) through plate pixel (px, py)."""
        x, y = px - PLATE_W / 2, py - PLATE_H / 2
        x, y = self._cr * x + self._sr * y, -self._sr * x + self._cr * y      # undo the roll
        # Level camera: right = +X, down = -Z, forward = +Y; then pitched down.
        return (x, self.focal_px * self._cp - y * self._sp, -self.focal_px * self._sp - y * self._cp)

    def ground_point(self, px, py, z=0.0):
        """World (x, y) of the surface at height `z` under plate pixel (px, py)."""
        dx, dy, dz = self.ray(px, py)
        t = (z - self.eye) / dz
        return (dx * t, dy * t)

    def project(self, x, y, z=0.0):
        """Plate pixel (px, py) of world point (x, y, z); inverse of ray()."""
        wz = z - self.eye
        depth = y * self._cp - wz * self._sp
        down = -y * self._sp - wz * self._cp
        right, down = self._cr * x - self._sr * down, self._sr * x + self._cr * down   # redo the roll
        return (PLATE_W / 2 + self.focal_px * right / depth,
                PLATE_H / 2 + self.focal_px * down / depth)
