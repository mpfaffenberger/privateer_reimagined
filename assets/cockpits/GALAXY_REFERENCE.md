# Galaxy cockpit (#462)

Mike supplied the original Privateer Galaxy cockpit reference:
https://www.wcnews.com/wcpedia/File:Privateer_-_Cockpit_-_Galaxy_-_Off.png

Actual PNG supplied as image-conditioning input:
https://www.wcnews.com/wcpedia/images/Privateer_-_Cockpit_-_Galaxy_-_Off.png

The 320x200 source screenshot belongs to its respective original creators;
it is retained only in ignored build reference storage, not distributed here.
The generated cockpit is an original higher-resolution reinterpretation of
its angular merchant-ship silhouette, with native transparent windows and
empty glass for live instruments. A second generation used both the first
draft and original screenshot to remove extra blank displays and improve
central canopy clearance.

- `galaxy_source.png`: final generated native-alpha source.
- `galaxy.png`: deterministic finalized overlay, 1672x940.
- Left small monitor: STATUS.
- Middle large monitor: RADAR / flight readouts.
- Right large monitor: NAV / TARGET.
- Far-right console is opaque switch hardware, not an unused transparent MFD.

```sh
python3 tools/cockpit_art.py finalize assets/cockpits/galaxy_source.png assets/cockpits/galaxy.png
python3 tools/cockpit_art.py analyze assets/cockpits/galaxy.png
```

Quads are measured in `cockpit_overlay_layout.h`. Full-canvas fit keeps the
ceiling attached to the viewport; rigid pilot slide, inverse mouse mapping,
and world-marker occlusion use the same shared cockpit renderer.
