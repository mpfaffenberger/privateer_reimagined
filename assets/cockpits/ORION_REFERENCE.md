# Orion cockpit (#729 / #730)

Mike picked the "Clean" sample from an image-conditioned generation run. It was
conditioned on two inputs:

- the original Privateer Orion cockpit, a 320x200 screenshot. That image belongs
  to its original creators and stays in local reference storage. It is not
  distributed here.
- `centurion_source.png`, for the shared gunmetal and brass house style.

The result is an original higher-resolution reinterpretation of the Orion's
heavy-gunship layout. It has two big lower VDUs, two canted monitors hanging
from the ceiling corners, and an AUTO strip at the top centre. The generator
produced native alpha, so there is no colour key. Every display is empty
transparent glass for live instruments.

- `orion_source.png`: Mike's chosen native-alpha source (1672x941), unmodified.
- `orion.png`: deterministic finalized overlay, 1672x940.

## Why `--pad-top 12`

The AUTO strip's glass starts 16 px below the top edge of the source. The
pilot-head motion zooms the overlay slightly about the screen centre and also
slides it. On windows up to about 1280 logical px wide, that pushed the top
2-5 px of the strip off-screen, and `test_cockpit_overlay` caught it.

The top 16 rows of the source are solid ceiling metal. `--pad-top 12` mirrors
the top 12 rows above the canvas, and the 16:9 trim then removes 12 rows of
keyboard from the bottom. The mirrored band sits almost entirely inside the
overscan, and the art itself is unchanged.

## Display roles

The Orion has five glass holes. `analyze` ranks holes by area, which does not
match the original layout, so the slots below are assigned by hand from its
measured quads:

| Glass                    | Slot       | Shows                                    |
|--------------------------|------------|------------------------------------------|
| Lower-left VDU           | `Left`     | STATUS                                   |
| Upper-right monitor      | `Center`   | RADAR, plus the SPD/mode and NRG flanks  |
| Lower-right VDU          | `Right`    | NAV / TARGET                             |
| Top-centre strip         | `Banner`   | Autopilot (the original's AUTO plate)    |
| Upper-left monitor       | `SetSpeed` | SET (commanded speed)                    |
| (none)                   | `Velocity` | Absent, so actual speed shows in the radar flank |

The original also shows SET on the upper left and KPS on the upper right, next
to the radar.

The other holes `analyze` reports are real canopy openings, not displays: the
main windscreen, two slots between the ceiling pipes, and a grab-handle loop.

```sh
python3 tools/cockpit_art.py finalize assets/cockpits/orion_source.png assets/cockpits/orion.png --pad-top 12
python3 tools/cockpit_art.py analyze assets/cockpits/orion.png
```

The quads are recorded in `cockpit_overlay_layout.h`. The Orion uses full-canvas
fit, like the Galaxy and Tarsus: its monitors hang from a physical ceiling, so a
boresight slide would open a band of space above it. The screen centre falls on
clear canopy between the VDUs.
