#!/usr/bin/env python3
"""Small deterministic anomaly summary for Level-4/replay CSV traces."""
from __future__ import annotations
import argparse, csv, math, statistics
from pathlib import Path

def vals(rows,key):
    out=[]
    for r in rows:
        try:
            x=float(r.get(key,'nan'))
            if math.isfinite(x): out.append(x)
        except ValueError: pass
    return out

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('csv',type=Path); a=ap.parse_args()
    with a.csv.open(newline='',encoding='utf-8') as f: rows=list(csv.DictReader(f))
    if not rows: raise SystemExit('empty trace')
    keys=['cadence_rpm','rider_torque_nm','torque_ckg','battery_v','battery_a','true_soc','fw_soc',
          'iq_request','iq_request_new','iq_ref','iq_ref_new','iq_actual','motor_erps','u_abs']
    print(f'TRACE {a.csv} rows={len(rows)}')
    for k in keys:
        x=vals(rows,k)
        if x:
            print(f'{k:18s} min={min(x):9.3f} max={max(x):9.3f} mean={statistics.fmean(x):9.3f} pp={max(x)-min(x):9.3f}')
    ts=vals(rows,'true_soc'); fs=vals(rows,'fw_soc')
    if ts and fs and len(ts)==len(fs):
        e=[abs(x-y) for x,y in zip(ts,fs)]; print(f'SOC abs error      max={max(e):.3f}% mean={statistics.fmean(e):.3f}%')
    # Replay delta columns make firmware changes immediately visible.
    for k in ['delta_request','delta_ref']:
        x=vals(rows,k)
        if x: print(f'{k:18s} maxAbs={max(map(abs,x)):.3f} meanAbs={statistics.fmean(map(abs,x)):.3f}')
if __name__=='__main__': main()
