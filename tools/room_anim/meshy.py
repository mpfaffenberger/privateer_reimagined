"""Make rigged, animated characters and static ship meshes with the Meshy AI API (#577, #699).

    uv run --with requests tools/room_anim/meshy.py character <name> --image ref.png [--height 1.7]
    uv run --with requests tools/room_anim/meshy.py animate <name> --action 33 --action 343
    uv run --with requests tools/room_anim/meshy.py library [--search sit]
    uv run --with requests tools/room_anim/meshy.py ship <name> --image hero.png --image side.png ...

`character` runs image-to-3D (A-pose, textured), remeshes it (meshy-7.1 makes
0.5-1.3M faces and rigging takes at most 300k), then auto-rigs it.
`animate` puts one library action on the rig per task, so every clip lands in
its own GLB: prep_character.py keeps one clip per file, and auditioning idles
side by side is the point. Feed a clip to prep_character.py to commit it.
`ship` runs multi-image-to-3D (1-4 views of one hull, e.g. a concept render
plus blueprints), symmetric and textured, with no rig.

Every task id is saved in build/room_anim/meshy/<name>/state.json before it
is polled, so a rerun resumes the task instead of paying for a new one
(image-to-3D 30 credits, remesh 5, rigging 5, each action 3, multi-image ~30).

The API key comes from MESHY_API_KEY or ~/.config/meshy/api_key and is never
printed or written anywhere else. Keep it out of the repo.
"""
import argparse
import base64
import json
import os
import sys
import time
from pathlib import Path

import requests

API = "https://api.meshy.ai/openapi/v1"
REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "build/room_anim/meshy"
POLL_S = 10


def _key():
    key = os.environ.get("MESHY_API_KEY") or ""
    path = Path.home() / ".config/meshy/api_key"
    if not key and path.exists():
        key = path.read_text().strip()
    if not key:
        sys.exit("no Meshy key: set MESHY_API_KEY or write ~/.config/meshy/api_key")
    return key


def _call(method, path, **kw):
    r = requests.request(method, f"{API}/{path}", timeout=120,
                         headers={"Authorization": f"Bearer {_key()}"}, **kw)
    if r.status_code >= 400:                    # the body is Meshy's message, never the key
        sys.exit(f"{method} {path}: {r.status_code} {r.text[:300]}")
    return r


def _data_uri(image):
    mime = "image/png" if image.suffix.lower() == ".png" else "image/jpeg"
    return f"data:{mime};base64,{base64.b64encode(image.read_bytes()).decode()}"


class Job:
    """One asset's working dir and task ledger."""

    def __init__(self, name):
        self.dir = OUT / name
        self.dir.mkdir(parents=True, exist_ok=True)
        self.ledger = self.dir / "state.json"
        self.state = json.loads(self.ledger.read_text()) if self.ledger.exists() else {}

    def task(self, slot, feature, body):
        """Create the task once (its id is saved first), then poll it to the end."""
        if slot not in self.state:
            self.state[slot] = _call("POST", feature, json=body).json()["result"]
            self.ledger.write_text(json.dumps(self.state, indent=2))
            print(f"[meshy] {slot}: created {feature} task {self.state[slot]}", flush=True)
        while True:
            t = _call("GET", f"{feature}/{self.state[slot]}").json()
            if t["status"] == "SUCCEEDED":
                print(f"[meshy] {slot}: done ({t.get('consumed_credits', '?')} credits)", flush=True)
                return t
            if t["status"] in ("FAILED", "CANCELED"):
                # Drop the id so a rerun tries again (failed tasks are refunded).
                del self.state[slot]
                self.ledger.write_text(json.dumps(self.state, indent=2))
                sys.exit(f"[meshy] {slot}: {t['status']} {t.get('task_error', {}).get('message', '')}")
            print(f"[meshy] {slot}: {t['status']} {t.get('progress', 0)}%", flush=True)
            time.sleep(POLL_S)

    def fetch(self, url, filename):
        path = self.dir / filename
        path.write_bytes(requests.get(url, timeout=300).content)
        print(f"[meshy] saved {path.relative_to(REPO)}", flush=True)
        return path


def character(args):
    ch = Job(args.name)
    model = ch.task("model", "image-to-3d", {
        "image_url": _data_uri(Path(args.image)),
        "ai_model": "latest",
        "should_texture": True,
        "texture_resolution": "2k",
        "pose_mode": "a-pose",                  # rigging wants a clean A/T pose
        "target_formats": ["glb"],
    })
    ch.fetch(model["model_urls"]["glb"], "model.glb")
    mesh = ch.task("remesh", "remesh", {"input_task_id": ch.state["model"],
                                        "target_polycount": args.polycount,
                                        "topology": "triangle", "target_formats": ["glb"]})
    ch.fetch(mesh["model_urls"]["glb"], "remeshed.glb")
    rig = ch.task("rig", "rigging", {"input_task_id": ch.state["remesh"],
                                     "height_meters": args.height})
    ch.fetch(rig["result"]["rigged_character_glb_url"], "rigged.glb")


def animate(args):
    ch = Job(args.name)
    if "rig" not in ch.state:
        sys.exit(f"no rig for {args.name}: run `character` first")
    names = {a["action_id"]: a["key"] for a in _library()}
    for action in args.action:
        # No fps post-process: it only rewrites the FBX, and glTF keys are in
        # seconds, so Blender resamples them to the scene's 24 fps on import.
        t = ch.task(f"action_{action}", "animations",
                    {"rig_task_id": ch.state["rig"], "action_id": action})
        ch.fetch(t["result"]["animation_glb_url"], f"{names.get(action, action)}.glb")


def ship(args):
    job = Job(args.name)
    body = {
        "image_urls": [_data_uri(Path(p)) for p in args.image],
        "ai_model": "latest",
        "symmetry_mode": "on",                 # hulls are bilateral; blueprints agree
        "should_remesh": True,
        "topology": "triangle",
        "target_polycount": args.polycount,
        "should_texture": True,
        "target_formats": ["glb"],
    }
    if args.texture_prompt:
        body["texture_prompt"] = args.texture_prompt
    model = job.task("model", "multi-image-to-3d", body)
    job.fetch(model["model_urls"]["glb"], "model.glb")


def _library():
    return _call("GET", "animations/library").json()


def library(args):
    for a in _library():
        if not args.search or args.search.lower() in (a["key"] + a["name"]).lower():
            print(f"{a['action_id']:4d}  {a['key']:40s} {a['category']}/{a.get('sub_category', '')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("character", help="image-to-3D, then auto-rig")
    c.add_argument("name")
    c.add_argument("--image", required=True, help="full-body, front-facing reference")
    c.add_argument("--height", type=float, default=1.7, help="metres")
    c.add_argument("--polycount", type=int, default=100_000,
                   help="remesh target, faces (rigging takes at most 300k)")
    a = sub.add_parser("animate", help="one GLB per library action")
    a.add_argument("name")
    a.add_argument("--action", type=int, action="append", required=True)
    lib = sub.add_parser("library", help="list library actions")
    lib.add_argument("--search")
    s = sub.add_parser("ship", help="multi-image-to-3D, static textured hull")
    s.add_argument("name")
    s.add_argument("--image", action="append", required=True,
                   help="1-4 views of the same hull (first one drives the texture)")
    s.add_argument("--polycount", type=int, default=60_000, help="remesh target, faces")
    s.add_argument("--texture-prompt", help="optional texture hint, max 600 chars")
    args = ap.parse_args()
    {"character": character, "animate": animate, "library": library,
     "ship": ship}[args.cmd](args)


if __name__ == "__main__":
    main()

