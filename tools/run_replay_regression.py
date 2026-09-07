#!/usr/bin/env python3
"""Replay infrastructure gate + registered real-ride regressions."""
from __future__ import annotations
import csv, hashlib, json, subprocess, sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]; B=R/'.build/replay'; B.mkdir(parents=True,exist_ok=True)

def run(*args): subprocess.run([sys.executable,*map(str,args)],cwd=R,check=True)
def sha(p:Path): return hashlib.sha256(p.read_bytes()).hexdigest()
def rows(p:Path):
    with p.open(newline='',encoding='utf-8') as f: return sum(1 for _ in f)-1

def smoke():
    src=R/'tests/host/out/RUN_60_ride.csv'; can=B/'smoke.canonical.csv'; a=B/'smoke.a.csv'; b=B/'smoke.b.csv'
    run('tools/import_ride_log.py',src,can); run('tools/run_replay.py',can,'--output',a); run('tools/run_replay.py',can,'--output',b)
    if rows(can)!=24000 or rows(a)!=24000: raise SystemExit('replay smoke row-count mismatch')
    if a.read_bytes()!=b.read_bytes(): raise SystemExit('replay nondeterministic: repeated output differs')
    print(f'REPLAY SMOKE PASS rows=24000 deterministic sha256={sha(a)}')

def registered():
    root=R/'sim/replay/cases'; count=0
    if not root.exists(): print('REPLAY registered cases: 0'); return
    for mf in sorted(root.glob('*/manifest.json')):
        m=json.loads(mf.read_text()); inp=mf.parent/m.get('input','input.csv'); out=B/(mf.parent.name+'.replayed.csv')
        run('tools/run_replay.py',inp,'--output',out)
        expected=m.get('accepted_output_sha256')
        if expected and sha(out)!=expected:
            raise SystemExit(f'replay regression changed: {mf.parent.name}: {sha(out)} != {expected}')
        print(f'REPLAY CASE PASS {mf.parent.name} rows={rows(out)} accepted={bool(expected)}')
        count+=1
    print(f'REPLAY registered cases: {count} PASS')

def main(): smoke(); registered()
if __name__=='__main__': main()
