"""EA-IFF-85 reader for Origin's Wing Commander: Privateer data files.

This module is intentionally low-level — it returns a recursive tree of
FORMs and chunks, leaving interpretation to per-asset modules
(cargo.py, ships.py, guns.py, …).

The IFF flavour Origin used in Privateer is mostly vanilla EA-IFF-85:
  - FORM/LIST/CAT  containers, big-endian sizes, 4-char IDs.
  - Chunks pad to 2-byte boundaries.
The Origin twist is that numeric fields *inside* a chunk body are little-endian
(typical PC software of the era). That conversion is handled by the per-asset
interpreters, not here.

References:
  - EA-IFF-85 spec (Jerry Morrison, 1985).
  - dpjudas/WCPrivateer/Sources/FileFormat/  (active reference recreation).
  - Mario "HCL" Brito's format notes at hcl.solsector.net.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator


# EA-IFF-85 container IDs. FORM is by far the most common in Privateer files;
# LIST/CAT appear occasionally for grouped assets.
_FORM_IDS = {"FORM", "LIST", "CAT "}


@dataclass
class Chunk:
    """A single IFF chunk.

    Leaf chunk → `body` is bytes, `form_type` is None, `children` is empty.
    FORM chunk → `body` is None, `form_type` is the 4-char form type
                 (e.g. "SYST", "CRGO", "REAL"), `children` lists sub-chunks.
    """

    id: str
    body: bytes | None = None
    form_type: str | None = None
    children: list["Chunk"] = field(default_factory=list)
    offset: int = 0  # byte offset in source file (debug aid)

    @property
    def is_form(self) -> bool:
        return self.form_type is not None

    def child(self, chunk_id: str) -> "Chunk | None":
        """First direct child whose id matches. None if not found."""
        for c in self.children:
            if c.id == chunk_id:
                return c
        return None

    def children_by_id(self, chunk_id: str) -> list["Chunk"]:
        """All direct children with matching id (in order)."""
        return [c for c in self.children if c.id == chunk_id]

    def find_all(self, chunk_id: str) -> Iterator["Chunk"]:
        """Recursive search — yields every chunk (any depth) with matching id."""
        for c in self.children:
            if c.id == chunk_id:
                yield c
            yield from c.find_all(chunk_id)

    def dump(self, depth: int = 0, max_body: int = 16) -> str:
        """Human-readable tree dump, for debugging."""
        pad = "  " * depth
        if self.is_form:
            head = f"{pad}FORM {self.form_type}  ({len(self.children)} children, @{self.offset:#x})"
            return "\n".join([head] + [c.dump(depth + 1, max_body) for c in self.children])
        body = self.body or b""
        preview = body[:max_body].hex(" ")
        more = "..." if len(body) > max_body else ""
        return f"{pad}{self.id}  ({len(body)}B, @{self.offset:#x})  {preview}{more}"


# ─────────────────────────────────────────────────────────────────────────────
# parser
# ─────────────────────────────────────────────────────────────────────────────


def parse_iff(data: bytes) -> Chunk:
    """Parse a complete IFF blob. Must start with FORM/LIST/CAT."""
    root, _end = parse_chunk_at(data, 0)
    return root


def _read_id(data: bytes, off: int) -> str:
    return data[off:off + 4].decode("ascii", errors="replace")


def _read_be_u32(data: bytes, off: int) -> int:
    return struct.unpack(">I", data[off:off + 4])[0]


def parse_chunk_at(data: bytes, off: int) -> tuple[Chunk, int]:
    """Parse one chunk starting at `off`. Returns (chunk, next_offset)."""
    chunk_id = _read_id(data, off)
    size = _read_be_u32(data, off + 4)
    body_start = off + 8
    body_end = body_start + size
    # IFF requires 2-byte alignment between chunks
    next_off = body_end + (body_end & 1)

    if chunk_id in _FORM_IDS:
        form_type = _read_id(data, body_start)
        chunk = Chunk(id=chunk_id, form_type=form_type, offset=off)
        cur = body_start + 4
        while cur < body_end:
            child, cur = parse_chunk_at(data, cur)
            chunk.children.append(child)
        return chunk, next_off

    body = data[body_start:body_end]
    return Chunk(id=chunk_id, body=body, offset=off), next_off


# ─────────────────────────────────────────────────────────────────────────────
# byte-level helpers commonly needed by interpreters
# ─────────────────────────────────────────────────────────────────────────────


def read_cstring(buf: bytes, start: int) -> tuple[str, int]:
    """Read a NUL-terminated ASCII string. Returns (text, offset_after_NUL).
    Raises ValueError if no NUL is found from `start` to end of buffer."""
    end = buf.find(b"\0", start)
    if end < 0:
        raise ValueError(f"no NUL terminator found from offset {start}")
    return buf[start:end].decode("ascii", errors="replace"), end + 1


def read_le_u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def read_le_u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def read_le_i16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<h", buf, off)[0]


# ─────────────────────────────────────────────────────────────────────────────
# CLI: dump an IFF tree (handy for exploring new file types)
# ─────────────────────────────────────────────────────────────────────────────


def _main() -> int:
    import sys
    if len(sys.argv) < 2:
        print("usage: python3 wc_iff.py <file.iff>", file=sys.stderr)
        return 2
    data = open(sys.argv[1], "rb").read()
    print(parse_iff(data).dump())
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
