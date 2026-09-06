#!/usr/bin/env python3
"""Portable whole-pipeline regression runner.

Mirrors tests/host/run_regression.ps1 with native gcc so Linux/CI executes the
same shipped C harnesses and production modules. It does not replace the
67-suite run_host_tests.py gate; it complements it with deterministic traces.
"""
from __future__ import annotations
import csv, math, os, statistics, subprocess, sys
from pathlib import Path

R = Path(__file__).resolve().parents[1]
T = R / 'tests/host'
C = T / 'common'
S = R / 'src'
I = R / 'inc'
STUB = C / 'host_stubs'
OUT = R / '.build/regression-linux'
OBJ = OUT / 'obj'
OUT.mkdir(parents=True, exist_ok=True)
OBJ.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'gcc')

TYPE_LIMITS = {'torque_input.c', 'assist_modes.c'}

def run(cmd, *, capture=False):
    p = subprocess.run(cmd, cwd=R, text=True,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.STDOUT if capture else None)
    if p.returncode:
        if capture and p.stdout: print(p.stdout)
        raise SystemExit(p.returncode)
    return p.stdout if capture else ''

def compile_obj(src: Path, obj: Path, incdirs, allow_type_limits=False):
    cmd=[CC,'-std=c11','-Wall','-Wextra','-Werror']
    if allow_type_limits: cmd += ['-Wno-type-limits']
    for d in incdirs: cmd += ['-I', str(d)]
    cmd += ['-c', str(src), '-o', str(obj)]
    run(cmd)

def build(name, harness_rel, common_files, modules, use_stubs=False):
    incdirs = ([STUB] if use_stubs else []) + [C, I]
    objs=[]
    ho=OBJ/f'{name}.harness.o'; compile_obj(T/harness_rel,ho,incdirs); objs.append(ho)
    for cf in common_files:
        o=OBJ/f'{name}.common.{Path(cf).name}.o'; compile_obj(C/cf,o,incdirs); objs.append(o)
    for m in modules:
        o=OBJ/f'{name}.src.{m}.o'; compile_obj(S/m,o,incdirs,m in TYPE_LIMITS); objs.append(o)
    exe=OBJ/name
    run([CC,'-o',str(exe),*[str(x) for x in objs],'-lm'])
    return exe

def read_csv(path):
    with open(path,newline='') as f: return list(csv.DictReader(f))

def fval(row,k):
    try:return float(row[k])
    except:return math.nan

def metric(rows,k):
    vals=[fval(r,k) for r in rows]
    vals=[v for v in vals if math.isfinite(v)]
    if not vals:return None
    vals_s=sorted(vals); n=len(vals_s)
    def pct(p):
        if n==1:return vals_s[0]
        x=(n-1)*p; a=int(math.floor(x)); b=int(math.ceil(x));
        return vals_s[a] if a==b else vals_s[a]+(vals_s[b]-vals_s[a])*(x-a)
    mean=sum(vals)/n
    return dict(Mean=mean,Min=min(vals),Max=max(vals),P5=pct(.05),P95=pct(.95),
                PeakToPeak=max(vals)-min(vals),StdDev=statistics.pstdev(vals),
                Ripple=(max(vals)-min(vals))/abs(mean) if mean else 0.0)

power_mod=['torque_input.c','rider_input.c','assist_modes.c','cadence_comp.c','power_curve.c',
           'assist_start.c','assist_extended_boost.c','tuning_config.c']
ride_mod=power_mod+['ride_control.c','fast_iq_slew.c','battery_iq_cap.c','ride_session.c',
                    'iq_chain.c','pedal_assist_gate.c','assist_dynamics.c','assist_limits.c','motor_core.c']
# ride_control now owns final 16 kHz mailbox and therefore must be linked with the same real modules
# used by the main host suites; this makes this portable runner current with FW139/FW140.

torque=build('torque_trace',Path('torque/torque_trace_host.c'),['crank_model.c'],['torque_input.c'])
power=build('power_pipeline',Path('pipeline/power_pipeline_host.c'),['crank_model.c'],power_mod)
ride=build('ride_control_pipeline',Path('pipeline/ride_control_pipeline_host.c'),
           ['crank_model.c','map_adapter.c','motor_service_stub.c'],ride_mod,True)
burst=build('missed_tick_burst',Path('scenarios/missed_tick_burst_host.c'),['crank_model.c'],
            ['torque_input.c','ride_episode.c'])

scenarios=['RUN_60','RUN_80','RUN_100','RUN_110','RUN_120','CADENCE_RAMP_50_120']
files={}
for sc in scenarios:
    for exe,tag in ((torque,'torque'),(power,'power'),(ride,'ride')):
        path=OUT/f'{sc}_{tag}.csv'; run([str(exe),sc,str(path)]); files[(sc,tag)]=path
burst_csv=OUT/'missed_tick_burst_summary.csv'; run([str(burst),str(burst_csv)])

# Exact deterministic rerun of a representative pipeline scenario.
repeat=OUT/'RUN_100_power_repeat.csv'; run([str(power),'RUN_100',str(repeat)])
base=(OUT/'RUN_100_power.csv').read_bytes(); rep=repeat.read_bytes()
if base != rep:
    print('FAIL: whole-pipeline determinism differs on identical RUN_100 rerun', file=sys.stderr)
    raise SystemExit(1)

power_cols=['torque_raw','torque_corrected','torque_fast','torque_run','human_power_w',
            'motor_power_raw_w','motor_power_w','iq_request']
ride_cols=['torque_fast','torque_run','iq_request','iq_final']
summary=[]
for sc in scenarios:
    for tag,cols in [('power',power_cols),('ride',ride_cols)]:
        rows=read_csv(files[(sc,tag)])
        for c in cols:
            m=metric(rows,c)
            if m: summary.append(dict(Scenario=sc,Layer=tag,Column=c,**m))
with open(OUT/'metrics_summary.csv','w',newline='') as f:
    fields=['Scenario','Layer','Column','Mean','Min','Max','P5','P95','PeakToPeak','StdDev','Ripple']
    w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(summary)

report=(
    'Portable whole-pipeline regression\n'
    '==================================\n'
    f'Scenarios: {len(scenarios)} x 3 layers = {len(scenarios)*3} traces\n'
    'Harnesses: torque_trace, power_pipeline, ride_control_pipeline, missed_tick_burst\n'
    'Determinism RUN_100 power rerun: PASS (byte-identical CSV)\n'
    'Build flags: -Wall -Wextra -Werror; documented type-limits exceptions only\n'
    'Result: PASS\n')
(OUT/'REPORT.txt').write_text(report)
print(report,end='')
print(OUT/'REPORT.txt')
