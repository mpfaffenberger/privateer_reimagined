"""Actors for the Refinery concourse's floor layers (#584, #623).

The ore train is the mining base's (#558, mining/actors.py): a yellow tug
towing a hopper heaped with ore, headlights and a turning amber beacon. Here
it delivers: in round the atrium's ring floor and into the cargo bay. The
refinery's floor is a ring, so it follows a curved path (FloorPath) instead
of the mining tunnel's straight line; the hopper follows the tug's tracks.

The pedestrians (#623) are ../walkers.py proxies; only their looks live here.
"""
import bisect
import importlib.util
import math

from base import TOOLS
from vehicles import sweep_beacon, trail


def _mining_actors():
    """mining/actors.py, loaded by path: both bases name their scene module
    `scene`, so it can't simply go on sys.path next to this one. It only
    needs the shared ships, stage and vehicles modules."""
    spec = importlib.util.spec_from_file_location("mining_actors",
                                                  TOOLS / "mining" / "actors.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


MINING = _mining_actors()
TRAIL = trail(MINING.HOPPER_LENGTH)       # m
# The atrium is dimmer and browner than the mining tunnel: the stock yellow
# glared against the painted drums, so the tug gets a heavier, cooler grime.
TUG_GRIME = (0.42, 0.4, 0.4)


def chaikin(points, passes=4):
    """Corner-cutting smoothing of an open polyline (ends kept)."""
    for _ in range(passes):
        out = [points[0]]
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            out += [(0.75 * x0 + 0.25 * x1, 0.75 * y0 + 0.25 * y1),
                    (0.25 * x0 + 0.75 * x1, 0.25 * y0 + 0.75 * y1)]
        points = out + [points[-1]]
    return points


class FloorPath:
    """A smooth (x, y) path on the floor, sampled by distance along it.
    Before its start and past its end it carries straight on, so a trailing
    car can sit behind the first point."""

    def __init__(self, points):
        self.pts = chaikin(points)
        self.cum = [0.0]
        for (x0, y0), (x1, y1) in zip(self.pts, self.pts[1:]):
            self.cum.append(self.cum[-1] + math.hypot(x1 - x0, y1 - y0))
        self.length = self.cum[-1]

    def at(self, s):
        """-> (x, y, heading) at distance s; heading turns +Y onto the path."""
        i = min(max(bisect.bisect_left(self.cum, s) - 1, 0), len(self.cum) - 2)
        (x0, y0), (x1, y1) = self.pts[i], self.pts[i + 1]
        seg = self.cum[i + 1] - self.cum[i]
        t = (s - self.cum[i]) / seg
        return x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, math.atan2(-(x1 - x0), y1 - y0)


def _key_along(obj, path, lag, speed, frames, fps, rumble):
    last = None
    for f in range(1, frames + 1):
        x, y, heading = path.at(speed * (f - 1) / fps - lag)
        if last is not None:                       # no 2 pi flips between keys
            heading += 2 * math.pi * round((last - heading) / (2 * math.pi))
        last = heading
        obj.location = (x, y, rumble * math.sin(f * 0.35))
        obj.rotation_euler = (0.0, 0.0, heading)
        obj.keyframe_insert("location", frame=f)
        obj.keyframe_insert("rotation_euler", frame=f)


def ore_train_along(path, speed, fps, beacon_rpm=40.0):
    """Build the ore train and drive it along `path` at `speed` m/s from
    frame 1 until the hopper reaches the end. -> ([tug, hopper], frames)."""
    tug, hopper, beacon = MINING.build_ore_train(tug_tint=TUG_GRIME)
    frames = 1 + math.ceil((path.length + TRAIL) / speed * fps)
    _key_along(tug, path, 0.0, speed, frames, fps, rumble=0.008)
    _key_along(hopper, path, TRAIL, speed, frames, fps, rumble=0.008)
    sweep_beacon(beacon, 1, frames, fps, beacon_rpm)
    return [tug, hopper], frames


# Pedestrians (#623): walkers.build_walker() proxies, 50-70 px tall on the
# ring floor. Dull, dirty working colours: the room is dim and brown, and
# anything brighter glared like the stock tug did.
LOOKS = {                            # layer: coat, trousers, skin, head
    # a refinery hand in a rust coverall and a scuffed hard hat
    "walker_ring": ((0.2, 0.075, 0.025), (0.17, 0.065, 0.022), (0.42, 0.3, 0.23),
                    (0.36, 0.27, 0.06)),
    # a clerk in a slate jacket, bareheaded
    "walker_bay": ((0.05, 0.06, 0.08), (0.09, 0.09, 0.1), (0.3, 0.2, 0.15), None),
}
