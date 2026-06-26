#!/usr/bin/env python3
"""Upload all voice-clone reference clips to MiniMax (purpose=voice_clone).

Reads assets/speech/clone_refs/manifest.json, uploads each .mp3 to
POST https://api.minimax.io/v1/files/upload, and records the returned file_id
plus a proposed (unique, valid) clone voice_id. Writes uploads.json.

Auth: MINIMAX_API_KEY env var (export it from ~/.zshrc before running).
Idempotent: skips entries already uploaded in uploads.json.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

REFS = Path("assets/speech/clone_refs")
UPLOADS = REFS / "uploads.json"
URL = "https://api.minimax.io/v1/files/upload"


def clone_voice_id(sid: str) -> str:
    """MiniMax custom voice_id: >=8 chars, starts with a letter, has a digit."""
    camel = "".join(w.capitalize() for w in re.split(r"[^A-Za-z0-9]+", sid) if w)
    vid = camel + "01"
    if not vid[0].isalpha():
        vid = "V" + vid
    return vid


def upload(path: Path, key: str) -> dict:
    r = subprocess.run(
        ["curl", "-s", "--request", "POST", "--url", URL,
         "--header", f"Authorization: Bearer {key}",
         "--form", "purpose=voice_clone",
         "--form", f"file=@{path}"],
        capture_output=True,
    )
    try:
        return json.loads(r.stdout.decode())
    except Exception:
        return {"_raw": r.stdout.decode()[:300], "_err": r.stderr.decode()[:200]}


def main() -> int:
    key = os.environ.get("MINIMAX_API_KEY")
    if not key:
        print("MINIMAX_API_KEY not set", file=sys.stderr)
        return 1
    manifest = json.loads((REFS / "manifest.json").read_text())
    done = {}
    if UPLOADS.exists():
        done = {e["id"]: e for e in json.loads(UPLOADS.read_text())}

    out = []
    ok = fail = 0
    for entry in manifest:
        sid = entry["id"]
        if sid in done and done[sid].get("file_id"):
            out.append(done[sid])
            continue
        resp = upload(Path(entry["ref"]), key)
        fid = (resp.get("file") or {}).get("file_id")
        status = (resp.get("base_resp") or {}).get("status_msg", resp.get("_raw", "?"))
        rec = dict(entry)
        rec["file_id"] = fid
        rec["voice_id"] = clone_voice_id(sid)
        rec["upload_status"] = status
        out.append(rec)
        if fid:
            ok += 1
            print(f"  ok   {sid:34} file_id={fid}  voice_id={rec['voice_id']}")
        else:
            fail += 1
            print(f"  FAIL {sid:34} {status}")
        UPLOADS.write_text(json.dumps(out, indent=1))  # save as we go
    print(f"\n[upload] {ok} uploaded, {fail} failed -> {UPLOADS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
