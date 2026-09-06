#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys
R=Path(__file__).resolve().parents[1]
out=R/'.build/sil'; out.mkdir(parents=True,exist_ok=True)
mods=[
 'src/torque_input.c','src/rider_input.c','src/assist_modes.c','src/cadence_comp.c',
 'src/power_curve.c','src/assist_start.c','src/assist_extended_boost.c','src/tuning_config.c',
 'src/ride_control.c','src/fast_iq_slew.c','src/battery_iq_cap.c','src/ride_session.c',
 'src/iq_chain.c','src/pedal_assist_gate.c','src/assist_dynamics.c','src/assist_limits.c',
 'src/motor_core.c','src/pas_quadrature.c','src/pas_direction.c','src/pas_sampler.c','src/pas_cadence.c',
 'tests/host/common/map_adapter.c','tests/host/common/motor_service_stub.c']
cmd=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-type-limits',
     '-Iinc','-Itests/host/common/host_stubs','-Itests/host/common','-o',str(out/'evist_sil'),
     'sim/evist_sil.c']+mods+['-lm']
p=subprocess.run(cmd,cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
if p.returncode:
 print(p.stdout); sys.exit(p.returncode)
r=subprocess.run([str(out/'evist_sil')],cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
print(r.stdout,end='')
(R/'.build/sil/REPORT.txt').write_text(r.stdout)
sys.exit(r.returncode)
