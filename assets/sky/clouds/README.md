# Parallax gas clouds (#704)

Monochrome wispy clouds. `src/sky_prop_renderer.cpp` tints them to the system's
nebula palette at draw time, and they sit on a virtual shell ~1.2 Mm out so they shift
as you fly (`autogen_sky_clouds` / `sky_prop_apparent` in `src/sky_props.cpp`).

Replacement art must be a 512x512 greyscale PNG, white wisps on **pure black**, with wide
black margins. It is blended additively.

Generated with codex_imagegen, then Lanczos-downscaled to 512 greyscale. Every prompt
started with:

> A single soft luminous interstellar gas cloud, rendered in MONOCHROME white and light grey only (no color at all), centered on a PURE SOLID BLACK background (#000000), with wide empty black margins so nothing touches the image edges; the gas fades gradually and smoothly into black at its borders, no stars, no points of light, no text, no frame. Painterly matte-painting style, delicate translucent wisps and filaments, subtle internal brightness variation, square composition. Shape: 

Shapes:
- `cloud_wisp_drift`: a long diagonal drifting veil of wispy filaments, like a torn curtain of smoke.
- `cloud_billow`: a soft rounded billowing cumulus-like puff with darker hollows and bright rims.
- `cloud_filament_arc`: a curved arc of thin fibrous filaments, like a faint supernova shock front fragment.
- `cloud_tattered`: a tattered irregular patch of thin fog with several separate small wisps around it.
