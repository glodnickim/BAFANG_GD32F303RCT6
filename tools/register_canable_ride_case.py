#!/usr/bin/env python3
"""Register a raw CANable FW145 .log directly as a permanent EVistDrive replay regression."""
from __future__ import annotations
import argparse, hashlib, json, shutil, subprocess, sys, tempfile
from datetime import datetime, timezone
from pathlib import Path
R=Path(__file__).resolve().parents[1]
def sha(p:Path)->str: return hashlib.sha256(p.read_bytes()).hexdigest()
def main()->int:
    ap=argparse.ArgumentParser()
    ap.add_argument('raw_log',type=Path)
    ap.add_argument('name')
    ap.add_argument('--accept-current',action='store_true',
                    help='store current replay output as accepted regression baseline; never use on known-bug behavior')
    ap.add_argument('--capture-hz',type=float,default=1_000_000.0)
    a=ap.parse_args()
    dst=R/'sim/replay/cases'/a.name
    if dst.exists(): raise SystemExit(f'case exists: {dst}')
    with tempfile.TemporaryDirectory() as td:
        pref=Path(td)/'decoded'
        subprocess.run([sys.executable,str(R/'tools/decode_canable_ride_log.py'),str(a.raw_log),
                        '--output-prefix',str(pref),'--capture-hz',str(a.capture_hz)],cwd=R,check=True)
        md=json.loads(Path(str(pref)+'.metadata.json').read_text())
        if md.get('status')!='PASS' or md.get('replayable_snapshots',0)<1:
            raise SystemExit(f"CANable capture is not replayable: {md.get('status')}")
        dst.mkdir(parents=True)
        shutil.copy2(Path(str(pref)+'.canonical.csv'),dst/'input.csv')
        shutil.copy2(Path(str(pref)+'.decoded.csv'),dst/'decoded_observations.csv')
        shutil.copy2(Path(str(pref)+'.metadata.json'),dst/'capture_metadata.json')
        # Preserve raw source bytes inside the case so the decoder can always be re-audited later.
        raw_name='source'+a.raw_log.suffix.lower()
        shutil.copy2(a.raw_log,dst/raw_name)
        manifest={
            'name':a.name,'input':'input.csv','source_name':a.raw_log.name,'source_file':raw_name,
            'source_sha256':sha(a.raw_log),'coverage':md.get('coverage',[]),'capture_schema_versions':md.get('schema_versions',[]),
            'capture_snapshots':md.get('snapshots',0),'capture_complete_ratio':md.get('snapshot_complete_ratio',0.0),
            'created_utc':datetime.now(timezone.utc).isoformat(),'accepted_output_sha256':None,
        }
        if a.accept_current:
            out=dst/'accepted_output.csv'
            subprocess.run([sys.executable,str(R/'tools/run_replay.py'),str(dst/'input.csv'),'--output',str(out)],cwd=R,check=True)
            manifest['accepted_output']='accepted_output.csv'; manifest['accepted_output_sha256']=sha(out)
        (dst/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f"REGISTER CANABLE PASS {a.name} snapshots={manifest['capture_snapshots']} accepted={a.accept_current} path={dst}")
    return 0
if __name__=='__main__': raise SystemExit(main())
