#!/usr/bin/env python3
"""Electrical + end-to-end EVistDrive SIL gate.

Two complementary binaries are built from production C:
1) foc_electrical_sil: real FOC.c + PI/current-loop + SVPWM + PWM geometry against a PMSM plant.
2) evist_full_foc_sil: real PAS/torque/assist/limits/final-Iq path plus the same real FOC/PMSM,
   with physical rotor motion generating Hall timing that is reconstructed by production rotor_angle.c.

The fast supervisory SIL remains separate because it can fuzz many more cases cheaply.
"""
from __future__ import annotations
import argparse, os, subprocess, sys
from pathlib import Path

R = Path(__file__).resolve().parents[1]
OUT = R / '.build' / 'electrical-sil'
OUT.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'gcc')

SUPERVISORY_MODULES = [
    'src/torque_input.c','src/rider_input.c','src/assist_modes.c','src/cadence_comp.c','src/cadence_filter.c',
    'src/power_curve.c','src/assist_start.c','src/assist_extended_boost.c','src/tuning_config.c',
    'src/ride_control.c','src/fast_iq_slew.c','src/battery_iq_cap.c','src/ride_session.c',
    'src/iq_chain.c','src/pedal_assist_gate.c','src/assist_dynamics.c','src/assist_limits.c',
    'src/motor_core.c','src/pas_quadrature.c','src/pas_direction.c','src/pas_liveness.c',
    'src/pas_sampler.c','src/pas_cadence.c','tests/host/common/map_adapter.c',
    'tests/host/common/motor_service_stub.c'
]
FOC_MODULES = ['src/FOC.c','src/foc_current_loop.c','src/pwm_geometry.c']
FULL_EXTRA = ['src/rotor_angle.c','src/quiet_zero.c']


def build(exe: Path, sources: list[str], *, full=False, sanitize=False) -> None:
    flags = ['-std=c11','-Wall','-Wextra','-Werror','-Wno-error=type-limits','-Wno-error=unused-parameter']
    flags += ['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitize else ['-O2']
    inc = ['-Isim/full_host_stubs','-Iinc']
    if full:
        flags += ['-DEVD_SIL_REAL_FOC=1']
        inc += ['-Itests/host/common']
    cmd = [CC,*flags,*inc,'-o',str(exe),*sources,'-lm']
    p = subprocess.run(cmd,cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if p.returncode:
        print(p.stdout, end='')
        raise SystemExit(p.returncode)
    if p.stdout:
        # Keep known production warnings visible without failing the gate; these are separately audited.
        print(p.stdout, end='')


def run(exe: Path, args: list[str], env=None) -> str:
    p = subprocess.run([str(exe),*args],cwd=R,text=True,stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT,env=env)
    print(p.stdout,end='')
    if p.returncode:
        raise SystemExit(p.returncode)
    return p.stdout


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--full-fuzz',type=int,default=1000,
                    help='end-to-end real-FOC/Hall randomized cases')
    ap.add_argument('--seed',default='0xE7157A39')
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--sanitize-fuzz',type=int,default=100)
    a=ap.parse_args()

    standalone=OUT/'foc_electrical_sil'
    build(standalone,['sim/foc_electrical_sil.c',*FOC_MODULES])
    report='STANDALONE REAL FOC / PMSM\n'+run(standalone,[])

    full=OUT/'evist_full_foc_sil'
    build(full,['sim/evist_sil.c',*SUPERVISORY_MODULES,*FOC_MODULES,*FULL_EXTRA],full=True)
    fixed=run(full,[])
    fuzz=run(full,['--fuzz',str(a.full_fuzz),a.seed])
    report += '\nEND-TO-END ASSIST -> REAL FOC -> PMSM -> HALL\n'+fixed+'\n'+fuzz

    if a.sanitize:
        san=OUT/'evist_full_foc_sil_asan'
        build(san,['sim/evist_sil.c',*SUPERVISORY_MODULES,*FOC_MODULES,*FULL_EXTRA],full=True,sanitize=True)
        env=os.environ.copy()
        env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1'
        env['UBSAN_OPTIONS']='halt_on_error=1'
        sout=run(san,['--fuzz',str(a.sanitize_fuzz),a.seed],env)
        report += '\nFULL FOC SANITIZERS ASan+UBSan\n'+sout

    path=OUT/'REPORT.txt'
    path.write_text(report)
    print(path)

if __name__=='__main__':
    main()
