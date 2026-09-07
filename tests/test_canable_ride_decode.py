#!/usr/bin/env python3
from __future__ import annotations
import csv, json, subprocess, sys, tempfile
from pathlib import Path
R=Path(__file__).resolve().parents[1]
BASE=0x10400

def u16(v): return [(v>>8)&255,v&255]
def i16(v): return u16(v&0xffff)
def line(us,ident,data):
    return f"[10:00:00]\t[INFO]\t{us}\tID:{ident:08X}\tDLC:8\tData:"+" ".join(f"{x:02X}" for x in data)+"\n"
def frame(tick,idx,payload): return u16(tick&0xffff)+payload

def main():
    with tempfile.TemporaryDirectory() as td:
        d=Path(td); raw=d/'ride.log'; prefix=d/'ride'
        tick=0x12345; us=1000000
        rows=[]
        rows.append(line(us,0x83106302,[0]*8)); us+=100 # ordinary CAN noise must be ignored
        payloads=[
          u16(987)+u16(321)+u16(654),
          i16(501)+i16(440)+i16(333),
          i16(-222)+i16(17)+u16(1444),
          u16(4123)+i16(1234)+u16(678),
          u16(2345)+u16(1701)+u16(0xA55A),
          [0x9B,0x42,(5|(2<<4)|(3<<6)),83,79,0],
          i16(-12345)+u16(234)+[(5|(1<<3)|(6<<4)|(1<<7)),(3|(2<<2)|(1<<4)|(1<<5)|(1<<6))],
        ]
        for idx,p in enumerate(payloads): rows.append(line(us,BASE+idx,frame(tick,idx,p))); us+=3000
        meta=[1,1,(tick>>24)&255,(tick>>16)&255,(tick>>8)&255,tick&255,0,7]
        rows.append(line(us,BASE+7,meta)); us+=3000
        # second snapshot: deliberately omit MOTOR. It must remain replayable, time must advance,
        # and metadata must report exactly one missing fragment instead of hiding the loss.
        tick2=tick+84
        for idx,p in enumerate(payloads):
            if idx==2: continue
            rows.append(line(us,BASE+idx,frame(tick2,idx,p))); us+=3000
        raw.write_text(''.join(rows))
        subprocess.run([sys.executable,str(R/'tools/decode_canable_ride_log.py'),str(raw),'--output-prefix',str(prefix)],cwd=R,check=True)
        meta=json.loads((d/'ride.metadata.json').read_text())
        assert meta['snapshots']==2 and meta['complete_snapshots']==1 and meta['replayable_snapshots']==2, meta
        assert meta['missing_frame_counts']['2']==1, meta
        assert meta['coverage']==['CORE','INTERNAL','LIMITS','MOTOR','ROTOR_PAS_STATE'], meta['coverage']
        with (d/'ride.decoded.csv').open(newline='') as f: dec=list(csv.DictReader(f))
        assert dec[0]['load_centikg']=='987' and dec[0]['iq_ref']=='333'
        assert dec[0]['battery_voltage_v']=='41.230000' and dec[0]['battery_current_a']=='12.340000'
        assert dec[0]['cadence_control_rpm']=='79' and dec[0]['qzero_state']=='3'
        assert dec[0]['hall_state']=='5' and dec[0]['pas_ab_snapshot']=='3'
        assert dec[1]['motor_erps']=='nan'
        with (d/'ride.canonical.csv').open(newline='') as f: can=list(csv.DictReader(f))
        assert len(can)==2
        assert can[0]['cadence_rpm']=='79' and can[0]['torque_ckg']=='987'
        assert can[0]['pas_ab']=='nan' and can[0]['pas_direction']=='-1'
        assert abs(float(can[1]['time_s'])-84/4000.0)<1e-9
        # Canonical output must be accepted by the native production-code replay harness.
        subprocess.run([sys.executable,str(R/'tools/run_replay.py'),str(d/'ride.canonical.csv'), '--output',str(d/'replayed.csv')],cwd=R,check=True)
        print('PASS CANable FW145 raw-log decode -> canonical -> native replay')
    return 0
if __name__=='__main__': raise SystemExit(main())
