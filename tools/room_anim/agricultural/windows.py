"""The Agricultural concourse's windows onto the farmland (#582).

Hand-traced glass of the centre and right panes, in plate px (1536x1024):
from the top of the plate down to the sill, inside the frames. The arch left
of the centre pane has a lit grey bevel that is as bright as the high sky, so
its edge is traced along the bevel rather than found by darkness. The left
pane (ringed planet, moon, reflections in the glass) and the top-left
skylight stay painted.

Shared by bake_sky.py (where the clouds may drift) and bake_traffic.py
(where aircraft outside may show). Pure PIL + numpy.
"""
import numpy as np
from PIL import Image, ImageDraw

CENTRE = [(668, 0), (1094, 0), (1094, 290), (586, 300), (588, 250), (600, 200), (610, 180),
          (617, 160), (626, 130), (636, 100), (645, 70), (655, 30)]
RIGHT = [(1123, 0), (1467, 0), (1466, 190), (1466, 268), (1123, 288)]
PANES = [CENTRE, RIGHT]
SUPERSAMPLE = 4


def glass(size=(1536, 1024)):
    """-> float32 (H, W) coverage of the window glass, antialiased."""
    w, h = size
    img = Image.new("L", (w * SUPERSAMPLE, h * SUPERSAMPLE), 0)
    draw = ImageDraw.Draw(img)
    for poly in PANES:
        draw.polygon([(x * SUPERSAMPLE, y * SUPERSAMPLE) for x, y in poly], fill=255)
    return np.asarray(img.resize(size, Image.BOX), np.float32) / 255.0
