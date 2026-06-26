#!/usr/bin/env python3
"""Clone every uploaded reference voice on MiniMax, then synthesize a test line.

Reads assets/speech/clone_refs/uploads.json (file_id + voice_id per speaker),
calls POST /v1/voice_clone for each, then POST /v1/t2a_v2 to render one common
test sentence per cloned voice into assets/speech/clone_tests/<voice_id>.mp3.

Auth: MINIMAX_API_KEY env var. Idempotent: skips voices whose test mp3 exists.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

CLONE_URL = "https://api.minimax.io/v1/voice_clone"
T2A_URL = "https://api.minimax.io/v1/t2a_v2"
UPLOADS = Path("assets/speech/clone_refs/uploads.json")
TESTS = Path("assets/speech/clone_tests")
TEST_LINE = ("This is the Gemini sector. Keep your guns loaded and your eyes "
             "open, pilot.")


def post(url, key, body):
    r = subprocess.run(
        ["curl", "-s", "--request", "POST", "--url", url,
         "--header", f"Authorization: Bearer {key}",
         "--header", "Content-Type: application/json",
         "--data", json.dumps(body)],
        capture_output=True,
    )
    try:
        return json.loads(r.stdout.decode())
    except Exception:
        return {"_raw": r.stdout.decode()[:200]}


def main() -> int:
    key = os.environ.get("MINIMAX_API_KEY")
    if not key:
        print("MINIMAX_API_KEY not set", file=sys.stderr)
        return 1
    TESTS.mkdir(parents=True, exist_ok=True)
    ups = json.loads(UPLOADS.read_text())
    results = []
    cloned = tts_ok = fail = 0
    for e in ups:
        sid, fid, vid = e["id"], e.get("file_id"), e.get("voice_id")
        if not fid:
            continue
        # 1) clone
        cl = post(CLONE_URL, key, {"file_id": fid, "voice_id": vid})
        cstat = (cl.get("base_resp") or {}).get("status_msg", cl.get("_raw", "?"))
        # 2) synthesize the test line
        out_mp3 = TESTS / f"{vid}.mp3"
        tstat = "skipped(exists)"
        if not out_mp3.exists():
            t = post(T2A_URL, key, {
                "model": "speech-2.8-hd", "text": TEST_LINE, "stream": False,
                "voice_setting": {"voice_id": vid, "speed": 1, "vol": 1, "pitch": 0},
                "audio_setting": {"sample_rate": 32000, "bitrate": 128000,
                                  "format": "mp3", "channel": 1},
                "output_format": "hex",
            })
            h = (t.get("data") or {}).get("audio", "")
            if h:
                out_mp3.write_bytes(bytes.fromhex(h))
                tstat = "ok"
            else:
                tstat = "FAIL:" + str((t.get("base_resp") or {}).get("status_msg",
                                                                     t.get("_raw")))
        rec = {"id": sid, "speaker": e.get("speaker"), "voice_id": vid,
               "file_id": fid, "clone_status": cstat, "tts_status": tstat,
               "test_mp3": str(out_mp3)}
        results.append(rec)
        if cstat == "success":
            cloned += 1
        if tstat in ("ok", "skipped(exists)"):
            tts_ok += 1
        else:
            fail += 1
        print(f"  {vid:26} clone={cstat:8} tts={tstat}")
        (TESTS / "results.json").write_text(json.dumps(results, indent=1))
        time.sleep(0.3)
    print(f"\n[clone] cloned={cloned} tts_ok={tts_ok} fail={fail} "
          f"-> {TESTS}/results.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
