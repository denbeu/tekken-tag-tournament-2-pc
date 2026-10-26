#!/usr/bin/env python3
"""Opt-in writer provenance trace synthetic gate and bounded-budget regression."""
import json,os,pathlib,subprocess,sys,tempfile
if len(sys.argv)!=2:raise SystemExit('usage <smoke.exe>')
exe=pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix="ttt2_writer_smoke_") as td:
    d=pathlib.Path(td)
    trigger=d/'start.trigger'
    output=d/'writer.jsonl'
    env=os.environ.copy()
    for name in ('TTT2_SIDE_CAMERA_WRITER','TTT2_SIDE_CAMERA_WRITER_OUT',
                 'TTT2_SIDE_CAMERA_TRIGGER'):env.pop(name,None)
    def run():
        proc=subprocess.run([str(exe)],env=env,text=True,capture_output=True,timeout=12)
        assert proc.returncode==0,(proc.returncode,proc.stdout,proc.stderr)
        return proc
    run()
    assert not output.exists(),'OFF must not create files'
    env['TTT2_SIDE_CAMERA_WRITER']='1'
    env['TTT2_SIDE_CAMERA_TRIGGER']=str(trigger)
    env['TTT2_SIDE_CAMERA_WRITER_OUT']=str(output)
    run()
    assert not output.exists(),'missing trigger must be inert'
    trigger.touch()
    run()
    rows=[json.loads(x)for x in output.read_text().splitlines()]
    assert len(rows)==120,len(rows)
    assert all(x['type']=='ppc_store_candidate' and x['raw'] in ('40094094','40740107','40031518') for x in rows)
    assert all(x['file']=='side_camera_writer_smoke.cpp' for x in rows)
    assert all(x['guest'] in ('00000000','00000004','00000008','0000000C','00000010','00000014',
                             '00000018','0000001C','00000020','00000024') for x in rows)
    saved=output.read_bytes()
    run()
    assert output.read_bytes()==saved,'existing log must never be overwritten'
print('Writer-provenance synthetic tests: PASS (OFF, no trigger, 120 records, site provenance, no overwrite)')
