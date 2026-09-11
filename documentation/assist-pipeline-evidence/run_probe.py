"""Read-only control audit: existing CANable serializers + existing native Controller Lab.
No device writes, no production changes, no accepted-baseline updates.
"""
import csv, io, json, subprocess, hashlib, copy, sys
from pathlib import Path

FW = Path(__file__).resolve().parents[2]
ROOT = FW.parent
OUT = FW / 'documentation' / 'assist-pipeline-evidence'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(FW))
from sim.controller_lab import server
status = server.ensure_built()
exe = server.EXE
node = r'C:\Program Files\nodejs\node.exe'
bridge = FW / 'sim/controller_lab/canable_bridge.js'
tuning = dict(tuning_schema_version=8, startup_boost_cadence_step=20,
    assist_run_deadband_mv=5, assist_hold_ms=1400, assist_min_iq_pct=2,
    assist_torque_run_window_deg=180, assist_start_steps=4,
    assist_torque_full_scale_centikg=6000, crank_length_mm=165)
level = dict(mode_type=1, support_ratio_pct=160, support_min_pct=80,
    support_max_pct=260, reference_power_w=200, progression_pct=50,
    curve_exponent_x10=15, curve_exponent_high_x10=15,
    emtb_parameter=140, emtb_based_on_power=True, emtb_reference_voltage_mv=36000,
    torque_assist_factor=120, max_motor_power_w=0, max_iq_pct=100,
    assist_without_rotation=False, minimum_pedal_load_kg=0.7,
    riding_minimum_pedal_load_kg=0.3, startup_boost_enabled=True,
    startup_boost_mode=0, startup_boost_strength_pct=100, startup_boost_end_rpm=27,
    smooth_start_enabled=False, smooth_start_ms=300, release_ms=100,
    power_rise_filter_ms=190, power_fall_filter_ms=450,
    iq_rise_slow_ms=325, iq_rise_fast_ms=190, iq_fall_slow_ms=525,
    iq_fall_fast_ms=125, extended_boost_trigger_load_kg=8,
    extended_boost_strength_pct=100, extended_boost_duration_ms=0)
base = dict(duration=10, cadence=72, torque=28, torque_ripple=45, asymmetry=8,
    speed=18, voltage=39, soc=55, assist=3, mode='keep', sample_ms=5,
    ride_start_s=0.5, ride_stop_s=8)
manifest = dict(source_hash=status['source_hash'], exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
    qualification='Forced inputs, production C. No FOC/PMSM response; u_abs and battery current fixed at zero.',
    cases=[])
allrows={}
def run(name, tc=None, lc=None, args=None):
    t={**tuning, **(tc or {})}; lv={**level, **(lc or {})}
    preset=dict(tuning=t, banks=[dict(bank_schema_version=8, bank_index=0,
        active_bank=0, wa_target_rpm=30, cadence_comp_enabled=False,
        levels=[copy.deepcopy(lv) for _ in range(5)])])
    b=subprocess.run([node,str(bridge)],input=json.dumps(preset),text=True,
        capture_output=True,check=True,cwd=FW)
    blobs=json.loads(b.stdout)
    cfg={**base,**(args or {})}
    cmd=[str(exe),*[f'{k}={v}' for k,v in cfg.items()],
        'tuning_blob='+blobs['tuning_blob'],'bank_blob='+blobs['bank_blob']]
    p=subprocess.run(cmd,text=True,capture_output=True,cwd=FW)
    if p.returncode:
        manifest['cases'].append(dict(name=name,error=p.stderr,exit_code=p.returncode))
        print(name,p.stderr.strip())
        return
    rows=list(csv.DictReader(io.StringIO(p.stdout)))
    steady=[r for r in rows if 3<=float(r['time_s'])<7.5]
    def span(key):
        vals=[float(r[key]) for r in steady]
        return dict(min=min(vals),max=max(vals),pp=max(vals)-min(vals),mean=sum(vals)/len(vals))
    metrics={k:span(k) for k in ['torque_fast_native','torque_run_native','iq_mode_request','iq_requested','iq_allowed','iq_ref']}
    metrics['max_ref_allowed_gap']=max(float(r['iq_ref'])-float(r['iq_allowed']) for r in rows)
    metrics['steady_ref_allowed_abs_gap']=max(abs(float(r['iq_ref'])-float(r['iq_allowed'])) for r in steady)
    metrics['steady_sessions']=sorted(set(r['session'] for r in steady))
    metrics['steady_debug_flags']=sorted(set(r['debug_flags'] for r in steady))
    after=[r for r in rows if float(r['time_s'])>=8]
    metrics['last_nonzero_ref_after_stop_s']=max((float(r['time_s']) for r in after if float(r['iq_ref'])>0),default=None)
    (OUT/(name+'.csv')).write_text(p.stdout,encoding='utf-8',newline='')
    manifest['cases'].append(dict(name=name,preset=preset,args=cfg,blobs=blobs,
        csv_sha256=hashlib.sha256(p.stdout.encode()).hexdigest(),metrics=metrics))
    allrows[name]=rows
    return metrics

for mode in [1,2,3,4,5,6]:
    for cadence in [40,72,110]:
        run(f'mode{mode}_cad{cadence}',lc={'mode_type':mode},args={'cadence':cadence})
for window in [0,30,180,360]: run(f'window{window}',tc={'assist_torque_run_window_deg':window})
run('deadband100',tc={'assist_run_deadband_mv':100})
run('power_filters5000',lc={'power_rise_filter_ms':5000,'power_fall_filter_ms':5000})
run('fall5000',lc={'iq_fall_slow_ms':5000,'iq_fall_fast_ms':5000})
run('without_rotation',lc={'assist_without_rotation':True},args={'cadence':0,'torque_ripple':0,'speed':0})
run('without_rotation_off',args={'cadence':0,'torque_ripple':0,'speed':0})
run('floor_above_profile',tc={'assist_min_iq_pct':25},lc={'max_iq_pct':1})
run('floor_default_profile1',lc={'max_iq_pct':1})
run('boost_end1',lc={'startup_boost_end_rpm':1},args={'cadence':20})
run('boost_end120',lc={'startup_boost_end_rpm':120},args={'cadence':20})
run('smooth_start',lc={'smooth_start_enabled':True,'smooth_start_ms':1000},args={'speed':0})
run('smooth_start_off',args={'speed':0})
run('repeat')
def identical(a,b,keys):
    return all(all(x[k]==y[k] for k in keys) for x,y in zip(allrows[a],allrows[b])) and len(allrows[a])==len(allrows[b])
keys=['torque_run_native','iq_mode_request','iq_requested','iq_allowed','iq_ref']
manifest['comparisons']={
    'window30_vs_180':identical('window30','window180',keys),
    'window180_vs_360':identical('window180','window360',keys),
    'deadband5_vs_100':identical('window180','deadband100',keys),
    'power_filters_default_vs_5000':identical('window180','power_filters5000',keys),
    'cadence_boost_end1_vs_120':identical('boost_end1','boost_end120',keys),
    'repeat_deterministic':identical('window180','repeat',list(allrows['repeat'][0]))}
manifest['live_settings_observation']=status.get('settings')
(OUT/'probe-results.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps({'comparisons':manifest['comparisons'],'cases':[{ 'name':c['name'],'iq':c['metrics']['iq_ref'],'gap':c['metrics']['steady_ref_allowed_abs_gap']} for c in manifest['cases'] if 'metrics' in c]},indent=2))
