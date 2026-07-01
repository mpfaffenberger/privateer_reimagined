#!/usr/bin/env python3
"""Generate full-screen base-room backgrounds with OpenAI gpt-image-2.

Original concept art (text-to-image) for new_privateer's painted base rooms.
Outputs a landscape PNG ready to drop in as a concourse background.

Usage:
    OPENAI_API_KEY=sk-... tools/gen_concourse_ai.py <room> <out.png>
where <room> is one of the keys in PROMPTS below (landing, concourse, ...).
"""
import base64, os, sys, time
import httpx

URL = "https://api.openai.com/v1/images/generations"

# Original prompts — a gritty asteroid-mining colony, hand-painted 90s sci-fi
# adventure-game vibe (NOT a copy of any existing game's art).
STYLE = ("Hand-painted retro sci-fi concept art, early-90s point-and-click "
         "adventure-game background, warm tungsten lighting, soft painterly "
         "brushwork, slightly grainy, cinematic wide establishing shot, no "
         "text, no UI, no characters.")
PROMPTS = {
    "landing": (
        "Interior of a cramped asteroid-mining station landing bay carved from "
        "ochre rock. A circular landing pad with glowing amber guide-lines and "
        "red marker strips on a metal deck, heavy industrial gantries and fuel "
        "rigs along the walls, a small satellite dish overhead, cargo crates "
        "stacked in alcoves. Empty pad, ready for a docked ship. " + STYLE),
    "concourse": (
        "Interior main concourse of an asteroid-mining colony: a long cavern "
        "tunnel of ochre rock ribbed with dark structural beams and rows of "
        "recessed ceiling lights, a cobblestone-tiled floor, side passages and "
        "archways leading to shops, distant mining machinery. Moody, lived-in. "
        + STYLE),
}

def gen(room, out_path):
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        sys.exit("Set OPENAI_API_KEY first.")
    if room not in PROMPTS:
        sys.exit(f"room must be one of {list(PROMPTS)}")
    payload = {"model": "gpt-image-2", "prompt": PROMPTS[room],
               "size": "1536x1024", "n": 1, "quality": "high"}
    t0 = time.monotonic()
    with httpx.Client(timeout=300) as c:
        r = c.post(URL, json=payload,
                   headers={"Authorization": f"Bearer {key}"})
        if r.status_code != 200:
            sys.exit(f"API error {r.status_code}: {r.text[:400]}")
        b64 = r.json()["data"][0]["b64_json"]
    open(out_path, "wb").write(base64.b64decode(b64))
    print(f"wrote {out_path} ({room}) in {time.monotonic()-t0:.0f}s")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: gen_concourse_ai.py <room> <out.png>")
    gen(sys.argv[1], sys.argv[2])
