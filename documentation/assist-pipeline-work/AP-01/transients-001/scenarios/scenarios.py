"""AP-01 transients-001 scenario definitions.

Every case starts from the ACCEPTED linear72 configuration (ap01_runner.TUNING / LEVEL / BASE,
imported, not retyped) and states its deviations explicitly in `args`. Nothing about the firmware
tuning is tweaked to make a waveform look better: the only tuning override anywhere in this file is
the one the accepted floor/profile regression cases already carried.

`events` lists the scripted event times used to build measurement windows. They are the COMMANDED
input times; the observed reaction time is measured separately and never assumed equal to them.
"""

# Baseline generator shape of the accepted AP-01 series.
BASE_RIPPLE = 45
BASE_ASYM = 8

STEP_LOW_NM = 15
STEP_HIGH_NM = 40
STEP_AT_S = 5.0
STEP_DURATION_S = 10
STEP_START_S = 0.5

RESTART_STOP_S = 5.0
RESTART_DURATION_S = 12
RESTART_START_S = 0.5
# Gap sweep. The "fast" restart is CHOSEN FROM THE OBSERVATION, not asserted: the PAS stop floor is
# 200 ms (PAS_STOP_TICKS=800 @4 kHz, inc/config.h) and the accepted level preset carries
# release_ms=100, so the interesting region is around 0.20-0.35 s - but whether a given gap really
# lands during the release / Iq decay is decided from iq_state_at_restart in the data.
RESTART_GAPS_S = [0.10, 0.15, 0.17, 0.19, 0.195, 0.20, 0.205, 0.21, 0.22, 0.25, 0.30, 0.50, 1.50]
RESTART_SPEEDS_KPH = [0, 18]


def step_cases() -> list[dict]:
    cases = []
    for cadence in (30, 72, 120):
        for direction, lo, hi in (("up", STEP_LOW_NM, STEP_HIGH_NM),
                                  ("down", STEP_HIGH_NM, STEP_LOW_NM)):
            cases.append(dict(
                name=f"step_{direction}_clean_cad{cadence}",
                group="torque_step_clean",
                purpose=("step measurement check without ripple/asymmetry: the underlying mean is "
                         "the only thing moving, so a 10-90% figure is unambiguous"),
                args=dict(duration=STEP_DURATION_S, cadence=cadence, torque=lo,
                          torque_ripple=0, asymmetry=0, speed=18,
                          ride_start_s=STEP_START_S, ride_stop_s=0,
                          torque_step_at_s=STEP_AT_S, torque_step_nm=hi),
                events=[("torque_step", STEP_AT_S)],
            ))
    for direction, lo, hi in (("up", STEP_LOW_NM, STEP_HIGH_NM),
                              ("down", STEP_HIGH_NM, STEP_LOW_NM)):
        cases.append(dict(
            name=f"step_{direction}_ripple_cad72",
            group="torque_step_ripple",
            purpose=("same step with the baseline pulsation and asymmetry: shows the limitation of "
                     "a single-crossing latency on a pulsating channel"),
            args=dict(duration=STEP_DURATION_S, cadence=72, torque=lo,
                      torque_ripple=BASE_RIPPLE, asymmetry=BASE_ASYM, speed=18,
                      ride_start_s=STEP_START_S, ride_stop_s=0,
                      torque_step_at_s=STEP_AT_S, torque_step_nm=hi),
            events=[("torque_step", STEP_AT_S)],
        ))
    return cases


def restart_cases() -> list[dict]:
    cases = []
    for speed in RESTART_SPEEDS_KPH:
        for gap in RESTART_GAPS_S:
            restart = round(RESTART_STOP_S + gap, 3)
            tag = f"{int(round(gap * 1000)):04d}ms"
            cases.append(dict(
                name=f"restart_gap{tag}_speed{speed}",
                group="stop_restart_rolling" if speed else "stop_restart_standing",
                purpose=("stop then restart after a scripted gap; which pipeline state the restart "
                         "actually lands in is read from the data, not assumed from the gap"),
                args=dict(duration=RESTART_DURATION_S, cadence=72, torque=28,
                          torque_ripple=BASE_RIPPLE, asymmetry=BASE_ASYM, speed=speed,
                          ride_start_s=RESTART_START_S, ride_stop_s=RESTART_STOP_S,
                          ride_restart_s=restart),
                events=[("ride_stop", RESTART_STOP_S), ("ride_restart", restart)],
            ))
    cases.append(dict(
        name="restart_then_stop2_speed18",
        group="stop_restart_second_stop",
        purpose=("second pedalling interval closed by ride_stop2_s: proves a steady window is "
                 "bounded by the NEXT event and that the first stop is not confirmed by the second"),
        args=dict(duration=RESTART_DURATION_S, cadence=72, torque=28,
                  torque_ripple=BASE_RIPPLE, asymmetry=BASE_ASYM, speed=18,
                  ride_start_s=RESTART_START_S, ride_stop_s=RESTART_STOP_S,
                  ride_restart_s=5.25, ride_stop2_s=8.0),
        events=[("ride_stop", RESTART_STOP_S), ("ride_restart", 5.25), ("ride_stop2", 8.0)],
    ))
    return cases


# AC-T2: the same three representative accepted cases, re-run in THIS directory with no new option
# set, to show that the extension leaves the old behaviour identical. The accepted CSVs in
# ../results/ are read for comparison only and are never rewritten.
REGRESSION_CASES = [
    dict(name="regr_linear72_baseline", accepted="linear72_baseline",
         args=dict(cadence=72), tc=None, lc=None),
    dict(name="regr_stop_release_cad72", accepted="stop_release_cad72",
         args=dict(cadence=72, ride_start_s=0.5, ride_stop_s=5.0), tc=None, lc=None),
    dict(name="regr_floor_above_profile", accepted="floor_above_profile_repro",
         args=dict(cadence=72), tc={"assist_min_iq_pct": 25}, lc={"max_iq_pct": 1}),
    dict(name="regr_floor_default_profile1", accepted="floor_default_profile1_repro",
         args=dict(cadence=72), tc=None, lc={"max_iq_pct": 1}),
]


def all_cases() -> list[dict]:
    return step_cases() + restart_cases()
