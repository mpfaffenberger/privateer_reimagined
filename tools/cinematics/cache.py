"""Content-hash cache for generated portraits.

Keyed on (character ref hash + full prompt + backend + size), so re-running a
retake with an unchanged prompt is FREE — no API spend. Cache blobs are stored
under ``tools/cinematics/.cache/<key>.png`` plus an ``index.json`` manifest for
human inspection. A hit copies the blob to the requested destination, so the
same generation reused at two different output paths costs one call.

The cache is intentionally dumb and file-based: no server, no eviction. Delete
``.cache/`` to force full regeneration.
"""
from __future__ import annotations

import hashlib
import json
import time
from pathlib import Path
from typing import Optional

CACHE_DIRNAME = ".cache"


class PortraitCache:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.dir = self.root / CACHE_DIRNAME
        self.dir.mkdir(parents=True, exist_ok=True)
        self.index_path = self.dir / "index.json"
        self._index = self._load_index()

    def _load_index(self) -> dict:
        if self.index_path.is_file():
            try:
                return json.loads(self.index_path.read_text())
            except Exception:
                return {}
        return {}

    def _save_index(self):
        self.index_path.write_text(json.dumps(self._index, indent=2))

    @staticmethod
    def key(*, prompt: str, backend: str, ref_bytes: Optional[bytes] = None,
            size: tuple = (512, 640)) -> str:
        h = hashlib.sha256()
        h.update(backend.encode("utf-8"))
        h.update(b"\x00")
        h.update(f"{size[0]}x{size[1]}".encode("utf-8"))
        h.update(b"\x00")
        h.update(prompt.encode("utf-8"))
        if ref_bytes is not None:
            h.update(b"\x00ref\x00")
            h.update(hashlib.sha256(ref_bytes).digest())
        return h.hexdigest()

    def get(self, key: str) -> Optional[bytes]:
        blob = self.dir / f"{key}.png"
        if blob.is_file():
            return blob.read_bytes()
        return None

    def put(self, key: str, png_bytes: bytes, meta: Optional[dict] = None):
        blob = self.dir / f"{key}.png"
        blob.write_bytes(png_bytes)
        entry = {"created": time.strftime("%Y-%m-%dT%H:%M:%S")}
        if meta:
            entry.update(meta)
        self._index[key] = entry
        self._save_index()
