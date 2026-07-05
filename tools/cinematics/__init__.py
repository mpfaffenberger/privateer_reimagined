"""Cinematic portrait generation pipeline (Phase 3).

Public library surface for the cinematic-director agent:

    from tools.cinematics.portraits import gen_line, STYLE_PREFIX
    from tools.cinematics import qc

The engine never imports any of this — it only loads pre-generated PNGs by
path. All AI/network code is confined to this package.
"""
