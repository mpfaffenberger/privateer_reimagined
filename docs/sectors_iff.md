# QUADRANT.IFF / SYST Chunk Format

Discovered by walking IFF chunks in `gog_extracted/extracted/priv/DATA/SECTORS/QUADRANT.IFF`.

## Outer layout

```
FORM UNIV                                       # outer wrapper
  INFO  (1 byte: quadrant count = 4)
  FORM QUAD  size=N
    INFO  (variable, see below)
    FORM SYST  size=N
      INFO  (variable, see below)
      BASE  size=N  (list of base IDs in this system)
```

## INFO chunk format

```
data = [ 4 bytes: x coord, 4 bytes: y coord, ?, 4 bytes: system name..., \0 ]
```

For SECT, the INFO contains a system name (variable length).

For QUAD, the INFO contains the quadrant name (variable length).

For UNIV, the INFO is a single byte: `0x04` (probably the number of quadrants).

## BASE chunk format

The BASE chunk contains a flat list of bytes — each byte is a `base_id`
from BASES.IFF that lives in this system. Size = number of bases.

Example: Junction has Burton (id 6), Speke (id 50), Victoria (id 56).

```
BASE size=3
       06 32 38     # 6, 50, 56 → Burton, Speke, Victoria
```

This is the only place on disk where star systems explicitly cross-reference
their bases (along with `assets/systems/<name>.json` in this project, which
is the same data reconstructed into JSON form).
