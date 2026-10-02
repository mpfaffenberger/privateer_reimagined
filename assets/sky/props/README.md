# Far-field sky props (#693)

Painterly galaxies and anomalies drawn on the celestial sphere by
`src/sky_prop_renderer.cpp`. The catalog (sizes, brightness, spin, pulse)
lives in `src/sky_props.cpp`. Each system seeds 1-3 of these from its
`skybox_seed`, or overrides them with `"sky": { "props": [...] }` in the system JSON.

Rules for replacement art:
- Square RGB PNG, 512x512, subject centred on **pure black** with wide black
  margins. Props are blended additively, so black adds nothing and a radial
  fade hides the card edge.
- Keep the stem; the catalog references `sky/props/<stem>`.

Generated with codex_imagegen at 1254x1254, then Lanczos-downscaled to 512. Every prompt
started with:

> Isolated astronomical object centered on a PURE SOLID BLACK background
> (#000000), with wide empty black margins on all sides so nothing touches the
> edges; the object fades smoothly into black, no stars, no frame, no text, no
> border, no vignette texture. Painterly 1990s space-sim matte-painting style
> (Wing Commander Privateer cover art vibe), rich luminous colors, soft glow,
> fine detail, square composition. Subject:

Subjects:
- `galaxy_spiral_blue`: a face-on grand-design spiral galaxy with two sweeping arms of blue-white star clusters, pink HII knots along the arms, warm golden core, dark dust lanes between the arms.
- `galaxy_barred_gold`: a face-on barred spiral galaxy with a bright golden elongated bar core and loose amber and cream spiral arms with faint blue outer star clusters.
- `galaxy_edgeon_dust`: an edge-on spiral galaxy seen exactly side-on as a thin glowing disc with a sharp dark dust lane through its middle and a bright bulging white-yellow core, tilted about 20 degrees.
- `galaxy_interacting_pair`: two colliding spiral galaxies linked by a long curved tidal tail of blue stars, one larger teal-white galaxy and one smaller orange one.
- `anomaly_pulsar_jets`: a tiny intensely bright blue-white neutron star pulsar with two thin straight opposing magenta and cyan energy jets and a small swirling glowing accretion disc.
- `anomaly_ring_nebula`: a planetary ring nebula: a glowing hollow oval ring of teal and orange ionised gas with a tiny white dwarf star at the center and faint outer filament shells.
- `anomaly_vortex_rift`: a spatial anomaly rift: a swirling violet and electric-green vortex funnel with a dark center ringed by bright lightning-like energy filaments.
- `anomaly_supernova_remnant`: a supernova remnant: a delicate tangled shell of red, gold and blue glowing gas filaments like the Crab nebula, with a bright point at the center.
