#!/usr/bin/env python3
"""Compare bounded TTT2 side-camera JSONL captures, without claiming camera identification.

Usage:
    python -B side_camera_compare.py idle=idle.jsonl apart=apart.jsonl strafe=strafe.jsonl
All observations are raw renderer constants; this tool does not infer world/camera matrices.
"""
import argparse
import collections
import json
import math
import pathlib
import struct

ROWS = (8, 9, 10, 11)
WORLD_ROWS = (20, 21, 22, 23)

def parse_rows(value):
    out = {}
    for group in value.split(";"):
        label, vals = group.split(":", 1)
        out[int(label.lstrip("c"))] = tuple(int(v, 16) for v in vals.split(","))
    if not all(row in out and len(out[row]) == 4 for row in ROWS + WORLD_ROWS):
        raise ValueError("missing or incomplete transform rows")
    return out

def unpack_float(u32):
    return struct.unpack("<f", struct.pack("<I", u32))[0]

def load(path):
    events = [json.loads(line) for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines() if line.strip()]
    types = [r.get("type") for r in events]
    if not events or types[0] != "start" or "end" not in types:
        raise ValueError(f"{path}: trace lacks complete start/end")
    frames = [e for e in events if e.get("type") == "frame_end"]
    draws = [e for e in events if e.get("type") == "draw"]
    writes = [e for e in events if e.get("type") == "write"]
    groups = collections.defaultdict(list)
    for draw in draws:
        rows = parse_rows(draw["constants"])
        mat = tuple(v for row in ROWS for v in rows[row])
        world = tuple(v for row in WORLD_ROWS for v in rows[row])
        key = draw["vs_hash"], draw["vs_count"]
        groups[key].append((mat,world,draw))
    provenance = collections.Counter((e["kind"],e["packet"],e["source"],e["row"]) for e in writes)
    end = next(e for e in reversed(events) if e.get("type") == "end")
    return dict(path=str(path), frames=len(frames),draws=len(draws),writes=len(writes),
                truncated=bool(end.get("truncated")),groups=groups,provenance=provenance)

def compare(items, limit=18):
    captures = {}
    for item in items:
        if "=" not in item:
            raise ValueError("each argument must be label=path")
        name, path = item.split("=",1)
        if not name or name in captures:
            raise ValueError("each capture needs unique label")
        captures[name] = load(path)
    print("Side camera candidate comparison (shader-constant evidence only)")
    for label, s in captures.items():
        print(f"  {label}: {s['frames']} frames, {s['draws']} draws, {s['writes']} c8..11 writes, truncated={s['truncated']}")
    by_key = collections.defaultdict(dict)
    for label,s in captures.items():
        for key,group in s["groups"].items():
            by_key[key][label] = group
    sorted_keys = sorted(by_key, key=lambda key:(-len(by_key[key]),-sum(len(x) for x in by_key[key].values()), key))
    print("\nVS hash / words | scenes | draws | distinct c8..11 by scene | distinct c20..23 by scene")
    for key in sorted_keys[:limit]:
        selected = by_key[key]
        view_counts = ", ".join(f"{lab}={len({x[0] for x in selected[lab]})}" for lab in captures if lab in selected)
        world_counts = ", ".join(f"{lab}={len({x[1] for x in selected[lab]})}" for lab in captures if lab in selected)
        print(f"{key[0]}/{key[1]} | {len(selected)} | {sum(len(x) for x in selected.values())} | {view_counts} | {world_counts}")
    print("\nRepresentative candidate row floats (first draw per VS / scene; not proven view):")
    for key in sorted_keys[:min(limit,5)]:
        for lab in captures:
            if lab not in by_key[key]:
                continue
            values = by_key[key][lab][0][0]
            floats = [unpack_float(v) for v in values]
            status = "finite" if all(math.isfinite(v) for v in floats) else "nonfinite"
            print(f"  vs={key[0]} scene={lab}: {status}: {['%.4g'%x for x in floats[:8]]}")
    print("\nMost frequent constant sources (source is GPU payload or LOAD buffer, not necessarily game camera object):")
    sources = collections.Counter()
    for lab,s in captures.items():
        sources.update((lab,src,kind,row) for (kind,pkt,src,row),count in s["provenance"].items() for _ in range(count))
    for (lab,src,kind,row),count in sources.most_common(12):
        print(f"  {lab}: src=0x{src} op=0x{kind:X} c{row} writes={count}")
    print("\nNo entry above identifies a camera without validation against projected world points.")

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures",nargs="+",help="label=trace.jsonl")
    parser.add_argument("--limit",type=int,default=18)
    args=parser.parse_args()
    compare(args.captures, max(1,args.limit))

if __name__ == "__main__":
    main()
