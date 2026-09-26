# Approved Tarsus cockpit (#458)

`tarsus_freighter_source.png` is the generated native-alpha source approved by Mike.
`tarsus_freighter.png` is its deterministic game-ready output:

```sh
python3 tools/cockpit_art.py finalize assets/cockpits/tarsus_freighter_source.png assets/cockpits/tarsus_freighter.png
python3 tools/cockpit_art.py analyze assets/cockpits/tarsus_freighter.png
```

## Image-conditioning references

Mike supplied https://www.wcnews.com/news/2261. The relevant article is
“Scout's Honor,” December 21, 2005, by LOAF, showing Howard Day's cockpit
for **Wing Commander: Pioneer**, not original 1993 Privateer artwork.
The generator received these three actual images as conditioning inputs:

- https://cdn.wcnews.com/newestshots/full/pioneer-tarsus21.jpg
- https://cdn.wcnews.com/newestshots/full/pioneer-tarsus22.jpg
- https://cdn.wcnews.com/newestshots/full/pioneer-tarsus23.jpg

Reference artwork belongs to its respective creators. The downloaded originals
are not bundled in game assets. The generated reinterpretation preserves the
asymmetric workstation silhouette with empty transparent screen glass for live
instruments, rather than copying baked screen graphics.

## Live mapping

- Suspended upper-left CRT: STATUS.
- Lower-left CRT: RADAR and flight readouts.
- Lower-right CRT: NAV / TARGET.

Three perspective quads and boresight are measured in `cockpit_overlay_layout.h`.
The full-width fit protects the hanging monitor in narrow windows. Existing
rigid slide, inverse mouse mapping and world-marker occlusion apply unchanged.
Legacy `tarsus.png` / raw assets are retained untouched.
