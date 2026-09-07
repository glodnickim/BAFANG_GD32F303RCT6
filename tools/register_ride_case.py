#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, json, shutil, subprocess, sys
from datetime import datetime, timezone
from pathlib import Path
R=Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('raw',type=Path); ap.add_argument('name'); ap.add_argument('--map',action='append',default=[]); ap.add_argument('--accept-current',action='store_true')
    a=ap.parse_args(); dst=R/'sim/replay/cases'/a.name
    if dst.exists(): raise SystemExit(f'case exists: {dst}')
    dst.mkdir(parents=True); inp=dst/'input.csv'; out=dst/'accepted_output.csv'; meta=dst/'import_metadata.json'
    cmd=[sys.executable,str(R/'tools/import_ride_log.py'),str(a.raw),str(inp),'--metadata',str(meta)]
    for x in a.map: cmd += ['--map',x]
    subprocess.run(cmd,cwd=R,check=True)
    im=json.loads(meta.read_text())
    manifest={'name':a.name,'input':'input.csv','source_name':a.raw.name,'source_sha256':sha(a.raw),'coverage':im.get('coverage',[]),
              'created_utc':datetime.now(timezone.utc).isoformat(),'accepted_output_sha256':None}
    if a.accept_current:
        subprocess.run([sys.executable,str(R/'tools/run_replay.py'),str(inp),'--output',str(out)],cwd=R,check=True)
        manifest['accepted_output_sha256']=sha(out); manifest['accepted_output']='accepted_output.csv'
    (dst/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'REGISTER PASS {a.name} accepted={a.accept_current} path={dst}')
if __name__=='__main__': main()
