"""AP-01 disturbances-001 scenario definitions.

Each case starts from the ACCEPTED linear72 configuration (ap01_runner.TUNING / LEVEL / BASE,
imported, not retyped) and states its deviations explicitly in `args`. Nothing about the firmware
tuning is tweaked to make a waveform look better.

`events` lists the scripted event times used to build measurement windows. They are the COMMANDED
input times; the observed reaction time is measured separately and never assumed equal to them.
An event time here MUST correspond to a real change in the generated input at that time - a
disturbance that is active from the first tick is NOT labelled with a later start (D010-04).

Every disturbance category is covered by three phases:
  1. a CONTROL case - identical run, disturbance keys unset;
  2. an INJECTION inside an already-settled ride;
  3. a RECOVERY - the window closes before the run ends and the input returns to normal.

Two INDEPENDENT timebase classes are kept apart on purpose:
  * miss_tick / fg_delay  - the PAS ISR keeps running, only the FOREGROUND call is deferred;
  * pas_edge_drop/jitter  - the ELECTRICAL PAS edge is lost or late, foreground runs normally.
"""
from __future__ import annotations

# Baseline generator shape of the accepted AP-01 series.
BASE_RIPPLE = 45
BASE_ASYM = 8

# Reverse / bounce
REVERSE_START_S = 3.0
REVERSE_END_S = 5.0
REVERSE_BOUNCE_AT_S = 4.0
BOUNCE_TICKS = 3
# 3 control ticks at 4 kHz is 0.00075 s, NOT 0.003 s. The old constant said "3 ticks" and
# carried 0.003, so every bounce metric window and every plot marker used an end time four
# times too late (REVIEW-EVD-AP-01-011, D011-01). Derived here so the two cannot disagree.
CTRL_HZ = 4000
BOUNCE_DURATION_S = BOUNCE_TICKS / CTRL_HZ  # = 0.00075 s

# Sensor invalid windows (must be >= 8 ticks for PAS invalid sequence)
TQ_INVALID_FROM_S = 3.0
TQ_INVALID_TO_S = 5.0
PAS_INVALID_FROM_S = 3.0
PAS_INVALID_TO_S = 5.0
PAS_INVSEQ_FROM_S = 3.0
PAS_INVSEQ_TO_S = 5.004  # 16 ticks = 4 ms @ 4 kHz, enough for 2 accepted transitions

# Electrical PAS edge disturbances (missed edge / late edge)
PAS_EDGE_FROM_S = 3.0
PAS_EDGE_TO_S = 5.0
PAS_EDGE_DROP_EVERY_N = 3
PAS_EDGE_JITTER_TICKS_VALUES = [2, 8]

# Timebase disturbances. miss_tick is now BOUNDED: control phase 0..2 s, injection 2..4 s,
# recovery 4..6 s. Previously it ran from the first tick while the metadata claimed start=1.0 s.
MISS_TICK_VALUES = [2, 4, 20]
MISS_TICK_FROM_S = 2.0
MISS_TICK_TO_S = 4.0
FG_DELAY_TICKS_VALUES = [1, 4, 20]
FG_DELAY_AT_S = 2.0

# RPM and speed sweep points
CADENCES = [30, 72]
SPEEDS_KPH = [0, 18]

# Control case durations
DURATION_REVERSE = 8.0
DURATION_INVALID = 8.0
DURATION_EDGE = 8.0
DURATION_TIMEBASE = 6.0

# Dedicated single-tick-resolution timebase probes. sample_ms=1 decimates to one row per 4 ticks
# at 4 kHz, which cannot show a single skipped tick; these use sample_ticks=1 instead (D010-03).
# Kept short so the full-rate CSV stays a reasonable size.
HIRES_DURATION = 1.5
# The stall must land in an ALREADY SETTLED ride, not on the ride_start_s edge itself - at
# ride_start_s the generator is still bringing torque and cadence up from zero, so a stall there
# would be measured against a transient rather than against steady pedalling.
HIRES_FG_DELAY_AT_S = 1.2


def _base_args(cadence: int, speed: int, duration: float) -> dict:
    return dict(
        duration=duration,
        cadence=cadence,
        torque=28,
        torque_ripple=BASE_RIPPLE,
        asymmetry=BASE_ASYM,
        speed=speed,
        voltage=39,
        soc=55,
        assist=3,
        mode="keep",
        sample_ms=1,
        ride_start_s=0.5,
        ride_stop_s=0,
    )


def control_cases() -> list[dict]:
    """Undisturbed reference runs - one per duration family, cadence and speed.

    A disturbance case is only readable against the SAME run without the disturbance, so these
    are required trials in their own right, not decoration.
    """
    cases = []
    for duration, tag in ((DURATION_REVERSE, "8s"), (DURATION_TIMEBASE, "6s")):
        for cadence in CADENCES:
            for speed in SPEEDS_KPH:
                cases.append(dict(
                    name=f"control_{tag}_cad{cadence}_speed{speed}",
                    group=f"control_{tag}",
                    purpose=("control: no disturbance key set at all; reference for every "
                             f"{tag} injection case at the same cadence and speed"),
                    args=_base_args(cadence, speed, duration),
                    events=[],
                ))
    return cases


def reverse_cases() -> list[dict]:
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            cases.append(dict(
                name=f"reverse_clean_cad{cadence}_speed{speed}",
                group="reverse_clean",
                purpose="clean reverse: crank angle goes negative, PAS quadrature follows signed index",
                args=dict(**_base_args(cadence, speed, DURATION_REVERSE),
                          reverse_at_s=REVERSE_START_S, forward_at_s=REVERSE_END_S),
                events=[("reverse_start", REVERSE_START_S), ("reverse_end", REVERSE_END_S)],
            ))
            cases.append(dict(
                name=f"reverse_bounce_cad{cadence}_speed{speed}",
                group="reverse_bounce",
                purpose="short 3-tick reverse line-state bounce without angle accumulation",
                args=dict(**_base_args(cadence, speed, DURATION_REVERSE),
                          reverse_bounce_at_s=REVERSE_BOUNCE_AT_S),
                events=[("reverse_bounce", REVERSE_BOUNCE_AT_S),
                        ("reverse_bounce_end", REVERSE_BOUNCE_AT_S + BOUNCE_DURATION_S)],
            ))
    return cases


def sensor_invalid_cases() -> list[dict]:
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            cases.append(dict(
                name=f"tq_invalid_cad{cadence}_speed{speed}",
                group="torque_invalid",
                purpose="torque_sensor_valid=false window; torque_input_update_elapsed receives sensor_valid=false",
                args=dict(**_base_args(cadence, speed, DURATION_INVALID),
                          torque_invalid_from_s=TQ_INVALID_FROM_S, torque_invalid_to_s=TQ_INVALID_TO_S),
                events=[("tq_invalid_start", TQ_INVALID_FROM_S), ("tq_invalid_end", TQ_INVALID_TO_S)],
            ))
            cases.append(dict(
                name=f"pas_invalid_cad{cadence}_speed{speed}",
                group="pas_invalid",
                purpose="pas_sensor_valid=false window; rider_input.pas_sensor_valid toggled, sampler still runs",
                args=dict(**_base_args(cadence, speed, DURATION_INVALID),
                          pas_invalid_from_s=PAS_INVALID_FROM_S, pas_invalid_to_s=PAS_INVALID_TO_S),
                events=[("pas_invalid_start", PAS_INVALID_FROM_S), ("pas_invalid_end", PAS_INVALID_TO_S)],
            ))
            cases.append(dict(
                name=f"pas_invseq_cad{cadence}_speed{speed}",
                group="pas_invalid_seq",
                purpose="illegal diagonal PAS quadrature injected for >= 8 ticks per transition; sampler accepts",
                args=dict(**_base_args(cadence, speed, DURATION_INVALID),
                          pas_invalid_seq_from_s=PAS_INVSEQ_FROM_S, pas_invalid_seq_to_s=PAS_INVSEQ_TO_S),
                events=[("pas_invseq_start", PAS_INVSEQ_FROM_S), ("pas_invseq_end", PAS_INVSEQ_TO_S)],
            ))
    return cases


def pas_edge_cases() -> list[dict]:
    """ELECTRICAL PAS edge disturbances: a missed edge and a late edge.

    Distinct from miss_tick/fg_delay: here the foreground runs on every tick and it is the PAS
    line itself that misbehaves, which is what the card asks for as a separate PAS class.
    """
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            cases.append(dict(
                name=f"pas_edge_drop{PAS_EDGE_DROP_EVERY_N}_cad{cadence}_speed{speed}",
                group="pas_edge_drop",
                purpose=(f"every {PAS_EDGE_DROP_EVERY_N}-rd PAS quadrature transition never reaches "
                         "the ISR; the next presented value is two steps away (illegal step)"),
                args=dict(**_base_args(cadence, speed, DURATION_EDGE),
                          pas_edge_drop_from_s=PAS_EDGE_FROM_S,
                          pas_edge_drop_to_s=PAS_EDGE_TO_S,
                          pas_edge_drop_every_n=PAS_EDGE_DROP_EVERY_N),
                events=[("pas_edge_drop_start", PAS_EDGE_FROM_S),
                        ("pas_edge_drop_end", PAS_EDGE_TO_S)],
            ))
            for ticks in PAS_EDGE_JITTER_TICKS_VALUES:
                cases.append(dict(
                    name=f"pas_edge_jitter{ticks}_cad{cadence}_speed{speed}",
                    group="pas_edge_jitter",
                    purpose=(f"every PAS transition reaches the ISR late, alternating {ticks} and "
                             f"{(ticks + 1) // 2} ticks of deferral; edge COUNT is preserved"),
                    args=dict(**_base_args(cadence, speed, DURATION_EDGE),
                              pas_edge_jitter_from_s=PAS_EDGE_FROM_S,
                              pas_edge_jitter_to_s=PAS_EDGE_TO_S,
                              pas_edge_jitter_ticks=ticks),
                    events=[("pas_edge_jitter_start", PAS_EDGE_FROM_S),
                            ("pas_edge_jitter_end", PAS_EDGE_TO_S)],
                ))
    return cases


def timebase_cases() -> list[dict]:
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            for n in MISS_TICK_VALUES:
                cases.append(dict(
                    name=f"miss_tick{n}_cad{cadence}_speed{speed}",
                    group="miss_tick",
                    purpose=(f"skip foreground every {n}-th 4 kHz tick, ONLY inside "
                             f"[{MISS_TICK_FROM_S}, {MISS_TICK_TO_S}) s; elapsed accumulates on resume"),
                    args=dict(**_base_args(cadence, speed, DURATION_TIMEBASE),
                              miss_tick_every_n=n,
                              miss_tick_from_s=MISS_TICK_FROM_S,
                              miss_tick_to_s=MISS_TICK_TO_S),
                    events=[("miss_tick_start", MISS_TICK_FROM_S),
                            ("miss_tick_end", MISS_TICK_TO_S)],
                ))
            for ticks in FG_DELAY_TICKS_VALUES:
                cases.append(dict(
                    name=f"fg_delay{ticks}_cad{cadence}_speed{speed}",
                    group="fg_delay",
                    purpose=(f"foreground stalled for {ticks} ticks starting at {FG_DELAY_AT_S}s; "
                             "16 kHz fast_iq_slew continues"),
                    args=dict(**_base_args(cadence, speed, DURATION_TIMEBASE),
                              fg_delay_at_s=FG_DELAY_AT_S, fg_delay_ticks=ticks),
                    events=[("fg_delay_start", FG_DELAY_AT_S),
                            ("fg_delay_end", FG_DELAY_AT_S + ticks / 4000.0)],
                ))
    return cases


HIRES_BOUNCE_AT_S = 1.2


def hires_bounce_cases() -> list[dict]:
    """The bounce at full 4 kHz row rate.

    The whole event is 3 ticks = 0.75 ms. At sample_ms=1 the CSV carries one row per 4 ticks, so
    the forced ticks are not individually observable and neither is the crank angle across them.
    These cases are what the bounce assertions and the zoom plot read.
    """
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            cases.append(dict(
                name=f"hires_bounce_cad{cadence}_speed{speed}",
                group="reverse_bounce_hires",
                purpose=("single 3-tick electrical PAS bounce at full 4 kHz row rate; the crank "
                         "keeps turning forward throughout - only the line is disturbed"),
                args=dict(**_base_args(cadence, speed, 2.0),
                          sample_ticks=1,
                          reverse_bounce_at_s=HIRES_BOUNCE_AT_S),
                events=[("reverse_bounce", HIRES_BOUNCE_AT_S),
                        ("reverse_bounce_end", HIRES_BOUNCE_AT_S + BOUNCE_DURATION_S)],
            ))
    return cases


def hires_bounce_control_cases() -> list[dict]:
    """The SAME run with no bounce key at all - the reference the bounce is compared against."""
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            cases.append(dict(
                name=f"hires_control_cad{cadence}_speed{speed}",
                group="control_hires",
                purpose="control for the full-rate bounce cases: identical run, no disturbance key",
                args=dict(**_base_args(cadence, speed, 2.0), sample_ticks=1),
                events=[],
            ))
    return cases


HIRES_REVERSE_AT_S = 1.0
HIRES_REVERSE_BACK_S = 1.5


def hires_reverse_cases() -> list[dict]:
    """Clean reverse at the FULL 4 kHz row rate - the record P-1 is measured on.

    REVIEW-EVD-AP-01-012 (D012-01): at sample_ms=1 the CSV carries one row per 4 ticks, so the
    first reverse line edge (and the inhibit that follows it 3 ticks later) fall between rows. The
    decimated record cannot resolve the latency and cannot even order the two events, so P-1 is
    measured here instead. No change to the C generator is needed - only sample_ticks=1.
    """
    cases = []
    for cadence in CADENCES:
        for speed in SPEEDS_KPH:
            cases.append(dict(
                name=f"hires_reverse_cad{cadence}_speed{speed}",
                group="reverse_clean_hires",
                purpose=("clean reverse at full 4 kHz row rate; the line steps backwards with no "
                         "override at all, so pas_ab == pas_normal_ab throughout"),
                args=dict(**_base_args(cadence, speed, 2.5),
                          sample_ticks=1,
                          reverse_at_s=HIRES_REVERSE_AT_S,
                          forward_at_s=HIRES_REVERSE_BACK_S),
                events=[("reverse_start", HIRES_REVERSE_AT_S),
                        ("reverse_end", HIRES_REVERSE_BACK_S)],
            ))
    return cases


def hires_timebase_cases() -> list[dict]:
    """Full 4 kHz row rate, so a SINGLE skipped tick is visible in the CSV itself."""
    cases = []
    for ticks in FG_DELAY_TICKS_VALUES:
        cases.append(dict(
            name=f"hires_fg_delay{ticks}_cad72_speed18",
            group="fg_delay_hires",
            purpose=(f"{ticks}-tick foreground stall at full 4 kHz row rate (sample_ticks=1): the "
                     "skipped ticks and the exact resume elapsed are visible per row, not aliased"),
            args=dict(**_base_args(72, 18, HIRES_DURATION),
                      sample_ticks=1,
                      fg_delay_at_s=HIRES_FG_DELAY_AT_S, fg_delay_ticks=ticks),
            events=[("fg_delay_start", HIRES_FG_DELAY_AT_S),
                    ("fg_delay_end", HIRES_FG_DELAY_AT_S + ticks / 4000.0)],
        ))
    return cases


# AC-T2 regressions: same presets as accepted AP-01, new keys unset
REGRESSION_CASES = [
    dict(name="regr_linear72_baseline", accepted="linear72_baseline",
         args=dict(cadence=72, sample_ms=5), tc=None, lc=None),
    dict(name="regr_stop_release_cad72", accepted="stop_release_cad72",
         args=dict(cadence=72, ride_start_s=0.5, ride_stop_s=5.0, sample_ms=5), tc=None, lc=None),
    dict(name="regr_floor_above_profile", accepted="floor_above_profile_repro",
         args=dict(cadence=72, sample_ms=5), tc={"assist_min_iq_pct": 25}, lc={"max_iq_pct": 1}),
    dict(name="regr_floor_default_profile1", accepted="floor_default_profile1_repro",
         args=dict(cadence=72, sample_ms=5), tc=None, lc={"max_iq_pct": 1}),
    dict(name="regr_step_up_clean_cad72", accepted="step_up_clean_cad72",
         args=dict(cadence=72, torque=15, torque_ripple=0, asymmetry=0, speed=18,
                   ride_start_s=0.5, ride_stop_s=0, sample_ms=5,
                   torque_step_at_s=5.0, torque_step_nm=40), tc=None, lc=None),
    dict(name="regr_restart_gap0195ms_speed18", accepted="restart_gap0195ms_speed18",
         args=dict(duration=12, cadence=72, torque=28, torque_ripple=45, asymmetry=8, speed=18,
                   ride_start_s=0.5, ride_stop_s=5.0, ride_restart_s=5.195, sample_ms=5), tc=None, lc=None),
    dict(name="regr_restart_gap0200ms_speed18", accepted="restart_gap0200ms_speed18",
         args=dict(duration=12, cadence=72, torque=28, torque_ripple=45, asymmetry=8, speed=18,
                   ride_start_s=0.5, ride_stop_s=5.0, ride_restart_s=5.200, sample_ms=5), tc=None, lc=None),
]


def all_cases() -> list[dict]:
    return (control_cases() + reverse_cases() + sensor_invalid_cases()
            + pas_edge_cases() + timebase_cases() + hires_timebase_cases()
            + hires_bounce_control_cases() + hires_bounce_cases()
            + hires_reverse_cases())


def all_regressions() -> list[dict]:
    return REGRESSION_CASES
