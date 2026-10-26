#!/usr/bin/env python3
"""Synthetic-only tests for optional guest PPC camera entry provenance."""
import json, os, pathlib, subprocess, sys, tempfile
if len(sys.argv)!=2:raise SystemExit("usage: test_side_camera_ppc_probe.py <harness-exe>")
exe=str(pathlib.Path(sys.argv[1]).resolve())
def invoke(env):
    p=subprocess.run([exe],env=env,capture_output=True,text=True,timeout=20)
    assert p.returncode==0,(p.stdout,p.stderr)
with tempfile.TemporaryDirectory(prefix="ttt2_camera_ppc_") as td:
    root=pathlib.Path(td)
    tr=root/"battle.trigger"; out=root/"producer.jsonl"
    env=os.environ.copy()
    for k in ("TTT2_SIDE_CAMERA_DIAG","TTT2_SIDE_CAMERA_PPC","TTT2_SIDE_CAMERA_PPC_OUT","TTT2_SIDE_CAMERA_TRIGGER","TTT2_SIDE_CAMERA_OUT"):
        env.pop(k,None)
    env["TTT2_SIDE_CAMERA_PPC_OUT"]=str(out)
    env["TTT2_SIDE_CAMERA_TRIGGER"]=str(tr)
    invoke(env)
    assert not out.exists()
    env["TTT2_SIDE_CAMERA_PPC"]="1"
    invoke(env)
    assert not out.exists(),"PPC trace must require trigger"
    tr.touch()
    invoke(env)
    data=[json.loads(x)for x in out.read_text(encoding="utf-8").splitlines()]
    assert len(data)==64,len(data)
    assert all(x.get("type")=="ppc" and isinstance(x.get("tick_ms"),int) for x in data)
    from collections import Counter
    counts=Counter(x["function"] for x in data)
    assert counts=={"0x82550C58":32,"0x82545458":32},counts
    assert data[0]["r3"]=="81000000"
    old=out.read_bytes()
    invoke(env)
    assert out.read_bytes()==old,"CREATE_NEW must never overwrite PPC log"
print("PPC camera producer synthetic tests: PASS (off, trigger, counts, 2 functions, JSON, no-overwrite)")
