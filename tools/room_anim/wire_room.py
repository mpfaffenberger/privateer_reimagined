"""Point one room's "layers" at a baked room, in every base (#578).

    uv run tools/room_anim/wire_room.py mercguild \\
        ../../shared_rooms/mercguild/merc_woman_patch.json ../../shared_rooms/mercguild/merc_woman.json

For rooms whose painting every base shares (the guilds): each base's
concourse.json gets the same list. The files are partly hand-formatted and
checked out with CRLF, so a JSON round-trip would reformat them: the list is
spliced in as text, after the room's "overlays" line (replacing an existing
"layers" array), in the file's own line endings. Parsing the result back
must show that room's layers changed and nothing else.
"""
import argparse
import json

from base import REPO

INDENT = "      "                            # a room's keys, under "rooms"


def splice(text, room, layers):
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    start = lines.index(f'    "{room}": {{')
    end = next(i for i in range(start + 1, len(lines)) if lines[i].startswith("    }"))
    block = lines[start:end]
    new = [f'{INDENT}"layers": ['] + [
        f'{INDENT}  {json.dumps(layer)}{"," if i < len(layers) - 1 else ""}'
        for i, layer in enumerate(layers)] + [f"{INDENT}],"]
    if f'{INDENT}"layers": [' in block:                # replace the old list
        a = start + block.index(f'{INDENT}"layers": [')
        b = next(i for i in range(a, end) if lines[i].startswith(f"{INDENT}]"))
        lines[a:b + 1] = new
    else:
        at = start + next(i for i, line in enumerate(block)
                          if line.startswith(f'{INDENT}"overlays"'))
        if not lines[at].endswith(","):
            raise SystemExit(f"{room}: overlays isn't followed by another key")
        lines[at + 1:at + 1] = new
    return nl.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("room")
    ap.add_argument("layers", nargs="+", help="paths relative to each base's dir")
    args = ap.parse_args()
    for path in sorted((REPO / "assets/concourse").glob("*/concourse.json")):
        text = path.read_bytes().decode("utf-8")
        after = splice(text, args.room, args.layers)
        old, new = json.loads(text), json.loads(after)
        if new["rooms"][args.room].get("layers") != args.layers:
            raise SystemExit(f"{path}: splice didn't take")
        old["rooms"][args.room]["layers"] = args.layers
        if old != new:
            raise SystemExit(f"{path}: splice changed more than {args.room}.layers")
        path.write_bytes(after.encode("utf-8"))
        print(f"[wire_room] {path.parent.name}: {args.room} -> {len(args.layers)} layers")


if __name__ == "__main__":
    main()
