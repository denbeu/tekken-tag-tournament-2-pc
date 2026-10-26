#!/usr/bin/env python3
"""Synthetic-only smoke test for side_camera_diag.cpp; never runs the game."""
import json, os, pathlib, subprocess, sys, tempfile

if len(sys.argv) != 2:
    raise SystemExit("usage: test_side_camera_diag.py <smoke-executable>")
exe = str(pathlib.Path(sys.argv[1]).resolve())
def run(env):
    p = subprocess.run([exe], env=env, text=True, capture_output=True, timeout=15)
    assert p.returncode == 0, (p.stdout, p.stderr)
with tempfile.TemporaryDirectory(prefix="ttt2_side_camera_") as tmp:
    root = pathlib.Path(tmp)
    env = os.environ.copy()
    env.pop("TTT2_SIDE_CAMERA_DIAG", None)
    env["TTT2_SIDE_CAMERA_OUT"] = str(root / "disabled.jsonl")
    env["TTT2_SIDE_CAMERA_TRIGGER"] = str(root / "ready.trigger")
    run(env)
    assert not pathlib.Path(env["TTT2_SIDE_CAMERA_OUT"]).exists(), "default should not write"
    env["TTT2_SIDE_CAMERA_DIAG"] = "1"
    env["TTT2_SIDE_CAMERA_OUT"] = str(root / "no_trigger.jsonl")
    run(env)
    assert not pathlib.Path(env["TTT2_SIDE_CAMERA_OUT"]).exists(), "trigger required"
    pathlib.Path(env["TTT2_SIDE_CAMERA_TRIGGER"]).touch()
    env["TTT2_SIDE_CAMERA_OUT"] = str(root / "capture.jsonl")
    env["TTT2_SIDE_CAMERA_FRAMES"] = "2"
    run(env)
    output = pathlib.Path(env["TTT2_SIDE_CAMERA_OUT"])
    assert output.stat().st_size < 1024*1024
    lines = [json.loads(row) for row in output.read_text("utf-8").splitlines()]
    types = [row["type"] for row in lines]
    for t in ("start", "write", "draw", "frame_end", "end"):
        assert t in types, (t, types)
    assert len([row for row in lines if row["type"] == "frame_end"]) == 2
    assert len([row for row in lines if row["type"] == "draw"]) == 2*128
    assert len([row for row in lines if row["type"] == "write"]) == 2*256
    assert lines[-1]["truncated"] is True and lines[-1]["frames"] == 2
    assert lines[-1]["omitted_draws"] == 44 and lines[-1]["omitted_writes"] == 48
    assert any(row["source"] == "0000A104" for row in lines if row["type"]=="write")
    assert all(row["vs_hash"] == "0000000000003039" for row in lines if row["type"]=="draw")
    original = output.read_bytes()
    run(env)
    assert output.read_bytes() == original, "CREATE_NEW must never overwrite an existing trace"
print("side camera synthetic smoke: PASS (disabled, trigger, provenance, caps, JSONL, create-new)")
