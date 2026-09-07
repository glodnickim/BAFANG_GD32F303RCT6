#!/usr/bin/env python3
from __future__ import annotations
import argparse, os, subprocess
from pathlib import Path
R=Path(__file__).resolve().parents[1]; OUT=R/'.build/replay'; OUT.mkdir(parents=True,exist_ok=True)
CC=os.environ.get('CC','gcc')
PROD=['src/torque_input.c','src/rider_input.c','src/assist_modes.c','src/cadence_comp.c','src/cadence_filter.c',
      'src/power_curve.c','src/assist_start.c','src/assist_extended_boost.c','src/tuning_config.c','src/ride_control.c',
      'src/fast_iq_slew.c','src/battery_iq_cap.c','src/ride_session.c','src/iq_chain.c','src/pedal_assist_gate.c',
      'src/assist_dynamics.c','src/assist_limits.c','src/motor_core.c','src/walk_assist_motor.c','src/walk_speed_controller.c',
      'tests/host/common/map_adapter.c','tests/host/common/motor_service_stub.c']
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('input',type=Path); ap.add_argument('--output',type=Path); ap.add_argument('--tolerance',type=float,default=-1)
    a=ap.parse_args(); exe=OUT/'replay_fw'; out=a.output or OUT/(a.input.stem+'.replayed.csv')
    cmd=[CC,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-error=type-limits','-Wno-error=unused-parameter',
         '-Isim/full_host_stubs','-Iinc','-Itests/host/common','-o',str(exe),'sim/replay/replay_fw.c',*PROD,'-lm']
    subprocess.run(cmd,cwd=R,check=True)
    args=[str(exe),str(a.input),str(out)]
    if a.tolerance>=0: args.append(str(a.tolerance))
    subprocess.run(args,cwd=R,check=True)
if __name__=='__main__': main()
