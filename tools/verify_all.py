#!/usr/bin/env python3
"""Single local quality gate for EVistDrive.

Runs the real-module host suites, deterministic whole-pipeline regression, closed-loop
supervisory SIL, stress fuzzing, ASan/UBSan and source-manifest hygiene. If the exact
Arm GNU toolchain + PowerShell are available, --target also performs the canonical debug
Developer target build using the existing production build script.
"""
from __future__ import annotations
import argparse, os, shutil, subprocess, sys, time
from pathlib import Path
R=Path(__file__).resolve().parents[1]

def step(name, cmd, env=None):
    print(f'\n=== {name} ===',flush=True); t=time.time()
    p=subprocess.run(cmd,cwd=R,env=env)
    if p.returncode:
        print(f'FAIL {name}: exit {p.returncode}',file=sys.stderr); raise SystemExit(p.returncode)
    print(f'PASS {name} ({time.time()-t:.1f}s)')

def manifest_gate():
    entries=[]
    for line in (R/'scripts/sources-m820.txt').read_text().splitlines():
        line=line.strip()
        if line and not line.startswith('#'): entries.append(line)
    missing=[x for x in entries if not (R/x).is_file()]
    product={x for x in entries if x.startswith('src/')}
    actual={p.relative_to(R).as_posix() for p in (R/'src').glob('*.c')}
    unlisted=sorted(actual-product)
    if missing or unlisted:
        print('missing manifest files:',missing); print('unlisted src/*.c:',unlisted); raise SystemExit(2)
    print(f'PASS source manifest: {len(product)}/{len(actual)} production C files listed; total entries={len(entries)}')

def diff_gate():
    if not (R/'.git').exists():
        print('SKIP git diff --check: exported tree has no .git'); return
    p=subprocess.run(['git','diff','--check'],cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if p.returncode: print(p.stdout); raise SystemExit(p.returncode)
    print('PASS git diff --check')

def target_build(require):
    gcc=shutil.which('arm-none-eabi-gcc') or shutil.which('arm-none-eabi-gcc.exe')
    ps=shutil.which('pwsh') or shutil.which('powershell') or shutil.which('powershell.exe')
    if not gcc or not ps:
        msg=f'TARGET BUILD SKIP: arm-none-eabi-gcc={bool(gcc)} PowerShell={bool(ps)}'
        if require: print(msg,file=sys.stderr); raise SystemExit(3)
        print(msg); return
    ver=subprocess.check_output([gcc,'-dumpfullversion','-dumpversion'],text=True).strip().splitlines()[0]
    if ver!='13.2.1':
        msg=f'TARGET BUILD SKIP: exact compiler 13.2.1 required, found {ver}'
        if require: print(msg,file=sys.stderr); raise SystemExit(3)
        print(msg); return
    toolbin=str(Path(gcc).resolve().parent)
    cmd=[ps]
    if Path(ps).name.lower().startswith('powershell'): cmd += ['-ExecutionPolicy','Bypass']
    cmd += ['-File',str(R/'scripts/build-firmware.ps1'),'-Target','M820_BL820','-Profile','debug',
            '-Variant','normal','-BuildMode','Developer','-Toolchain',toolbin,
            '-OutputDir',str(R/'.build/verify-target')]
    step('exact ARM target build',cmd)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--quick',action='store_true',help='1000 fuzz, skip sanitizers')
    ap.add_argument('--target',action='store_true',help='attempt exact ARM target build when tools exist')
    ap.add_argument('--require-target',action='store_true',help='fail if exact target toolchain cannot run')
    a=ap.parse_args()
    print('EVistDrive full verification gate')
    manifest_gate(); diff_gate()
    step('68 real-module host suites',[sys.executable,'tools/run_host_tests.py'])
    step('whole-pipeline deterministic regression',[sys.executable,'tools/run_regression.py'])
    sil=[sys.executable,'tools/run_sil.py','--fuzz','1000' if a.quick else '10000']
    if not a.quick: sil += ['--sanitize','--sanitize-fuzz','1000']
    step('closed-loop SIL + deterministic fuzz'+('' if a.quick else ' + ASan/UBSan'),sil)
    if a.target or a.require_target: target_build(a.require_target)
    print('\n==================================================')
    print('EVistDrive PC VERIFICATION: PASS')
    print('Target build is a separate gate; use --target / --require-target with Arm GNU 13.2.1.')
    print('==================================================')

if __name__=='__main__': main()
