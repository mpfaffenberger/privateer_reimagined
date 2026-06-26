#!/usr/bin/env python3
"""Transcribe the bar/fixer speech WAVs via a local faster-whisper server.

POSTs every assets/speech/bar/*.wav to an OpenAI-compatible
/v1/audio/transcriptions endpoint and collects the results into a single JSON
manifest. Resumable (skips files already in the manifest) and concurrent.

Usage:
  python3 tools/transcribe_bar_speech.py \
      --dir assets/speech/bar \
      --server http://192.168.1.252:8000 \
      --out assets/speech/bar_transcripts.json \
      --workers 4
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


def transcribe_one(wav: Path, server: str, timeout: int) -> dict:
    """Return {text, language, duration} or {error} for one file via curl."""
    url = f"{server.rstrip('/')}/v1/audio/transcriptions"
    r = subprocess.run(
        ["curl", "-s", "-m", str(timeout),
         "-F", f"file=@{wav}",
         "-F", "response_format=json",
         url],
        capture_output=True,
    )
    if r.returncode != 0:
        return {"error": f"curl rc={r.returncode}: {r.stderr.decode()[:120]}"}
    try:
        d = json.loads(r.stdout.decode())
    except Exception as e:
        return {"error": f"bad json: {e}: {r.stdout[:120]!r}"}
    if "text" not in d:
        return {"error": f"no text in response: {str(d)[:160]}"}
    return {
        "text": d["text"].strip(),
        "language": d.get("language"),
        "duration": d.get("duration"),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=Path("assets/speech/bar"))
    ap.add_argument("--server", default="http://192.168.1.252:8000")
    ap.add_argument("--out", type=Path, default=Path("assets/speech/bar_transcripts.json"))
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    wavs = sorted(args.dir.glob("*.wav"))
    if not wavs:
        print(f"no WAVs in {args.dir}", file=sys.stderr)
        return 1

    results: dict[str, dict] = {}
    if args.out.exists():
        results = json.loads(args.out.read_text())
    todo = [w for w in wavs if w.name not in results or "error" in results[w.name]]
    print(f"[transcribe] {len(wavs)} WAVs, {len(wavs) - len(todo)} already done, "
          f"{len(todo)} to do via {args.server}")

    lock = threading.Lock()
    done = 0
    errors = 0

    def save():
        tmp = args.out.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(results, indent=1, sort_keys=True))
        tmp.replace(args.out)

    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(transcribe_one, w, args.server, args.timeout): w
                for w in todo}
        for fut in as_completed(futs):
            w = futs[fut]
            res = fut.result()
            with lock:
                results[w.name] = res
                done += 1
                if "error" in res:
                    errors += 1
                    print(f"  ! {w.name}: {res['error']}", file=sys.stderr)
                if done % 50 == 0 or done == len(todo):
                    save()
                    print(f"  ...{done}/{len(todo)} done ({errors} errors)")

    save()
    ok = sum(1 for r in results.values() if "text" in r)
    print(f"[transcribe] complete: {ok} transcribed, "
          f"{len(results) - ok} errors -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
