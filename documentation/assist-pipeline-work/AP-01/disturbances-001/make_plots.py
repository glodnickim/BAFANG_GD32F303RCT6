"""AP-01 disturbances-001: whole-path plots with the events marked.

Self-contained SVG, no plotting library (matplotlib is not installed in this environment and a
measurement artefact should not depend on one). Every sample that lands in the plotted time range
is drawn: NO smoothing, NO decimation, NO averaging.

One figure per case, stacked panels covering the WHOLE path:
  1  generator input      torque_gen_nm, torque_cmd_mean_nm, cadence_gen_rpm
  2  conditioned rider    torque_run_native, cadence_control_rpm
  3  Iq chain             iq_before_profile_limit, iq_mode_request, iq_requested, iq_allowed, iq_ref
  4  supervisory state    session, gate_steps, debug_flags, ride_interval, crank_direction, pas_transition_index, pas_direction_state, pas_inhibit_reason, fg_delay_active, reverse_bounce_active, pas_invalid_seq_active
  5  sensor validity      torque_sensor_valid, pas_sensor_valid, rider_torque_sensor_valid, rider_pas_sensor_valid
  6  timebase             elapsed_ticks, fg_processed, fg_delay_active, reverse_bounce_active, pas_invalid_seq_active, fast_iq_slew_ticks

Event times are drawn as labelled vertical rules. They are the COMMANDED input times; the observed
reaction is in disturbances-metrics.json and is generally later.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import disturbance_metrics as em  # noqa: E402 - accepted event_metrics, loaded by path
import csv_contract  # noqa: E402

RESULTS = HERE / "results"
PLOTS = HERE / "plots"
PLOTS.mkdir(exist_ok=True)

W, H = 1320, 200          # panel width/height in px
PAD_L, PAD_R, PAD_T, PAD_B = 78, 300, 26, 34

LEGEND_X_OFFSET = 33
LEGEND_FS = 10
LEGEND_CHAR_W = LEGEND_FS * 0.6
LEGEND_MAX_CHARS = int((PAD_R - LEGEND_X_OFFSET) / LEGEND_CHAR_W)

# Panels are grouped BY SCALE, not merely by topic. A 0/1 flag and a cumulative 19 200-count
# tick counter share no usable y-axis: drawn together, the flag is a flat line on the baseline and
# the plot says nothing. Each panel below spans one order-of-magnitude family.
PANELS = [
    ("Generator input (synthetic rider)", [
        ("torque_gen_nm", "#1f77b4", "torque_gen_nm [Nm]"),
        ("torque_cmd_mean_nm", "#d62728", "torque_cmd_mean_nm [Nm] (commanded mean)"),
        ("cadence_gen_rpm", "#2ca02c", "cadence_gen_rpm [rpm]"),
    ]),
    ("Conditioned rider input (production; torque is in native sensor counts, not Nm)", [
        ("torque_run_native", "#1f77b4", "torque_run_native [native counts, ~27/kg]"),
        ("cadence_control_rpm", "#2ca02c", "cadence_control_rpm [rpm]"),
    ]),
    ("Iq chain (production, Iq request units - not Nm, not amps)", [
        ("iq_before_profile_limit", "#9467bd", "iq_before_profile_limit [Iq counts]"),
        ("iq_mode_request", "#8c564b", "iq_mode_request [Iq counts]"),
        ("iq_requested", "#e377c2", "iq_requested [Iq counts]"),
        ("iq_allowed", "#ff7f0e", "iq_allowed [Iq counts]"),
        ("iq_ref", "#d62728", "iq_ref [Iq counts] (final, 16 kHz)"),
    ]),
    # debug_flags is a BITMASK reaching 144 while session spans 0..2. Sharing one axis made
    # session 1.4% of the panel height - a state change lasting milliseconds was invisible
    # (REVIEW-EVD-AP-01-011, D011-04). The bitmask now has its own panel.
    ("Supervisory state (small enums)", [
        ("session", "#1f77b4", "session [enum 0-3]"),
        ("gate_steps", "#2ca02c", "gate_steps [counts]"),
        ("ride_interval", "#7f7f7f", "ride_interval [0/1/2] (generator)"),
        ("crank_direction", "#d62728", "crank_direction [-1/0/1] (generator)"),
        ("pas_direction_state", "#8c564b", "pas_direction_state [0=SAFE,1=INHIBIT,2=CONFIRM]"),
        ("pas_inhibit_reason", "#e377c2", "pas_inhibit_reason [0=none,1=rev,2=inv]"),
    ]),
    ("debug_flags (bitmask - own axis, see legend below)", [
        ("debug_flags", "#ff7f0e", "debug_flags [bitmask]"),
    ]),
    ("PAS position and run lengths", [
        ("pas_transition_index", "#9467bd", "pas_transition_index [signed quadrature index]"),
        ("pas_fwd_run", "#2ca02c", "pas_fwd_run [consecutive forward steps]"),
        ("pas_rev_run", "#d62728", "pas_rev_run [consecutive reverse steps]"),
    ]),
    ("Sensor validity (commanded vs stored) and injection flags [all 0/1]", [
        ("torque_sensor_valid", "#1f77b4", "torque_sensor_valid [commanded]"),
        ("pas_sensor_valid", "#ff7f0e", "pas_sensor_valid [commanded]"),
        ("rider_torque_sensor_valid", "#2ca02c", "rider_torque_sensor_valid [stored in rider_input]"),
        ("rider_pas_sensor_valid", "#d62728", "rider_pas_sensor_valid [stored in rider_input]"),
        ("pas_backpedal_confirmed", "#9467bd", "pas_backpedal_confirmed [production]"),
        ("fg_delay_active", "#8c564b", "fg_delay_active [generator]"),
        ("reverse_bounce_active", "#e377c2", "reverse_bounce_active [generator]"),
        ("pas_invalid_seq_active", "#7f7f7f", "pas_invalid_seq_active [generator]"),
        ("pas_edge_drop_active", "#17becf", "pas_edge_drop_active [generator]"),
        ("pas_edge_jitter_active", "#bcbd22", "pas_edge_jitter_active [generator]"),
    ]),
    ("Timebase, per-call scale [4 kHz ticks]", [
        ("elapsed_ticks", "#1f77b4", "elapsed_ticks [ticks passed to torque_input/ride_control]"),
        ("fg_last_resume_elapsed", "#d62728", "fg_last_resume_elapsed [latched, survives decimation]"),
        ("fg_processed", "#ff7f0e", "fg_processed [0=skipped, 1=processed]"),
    ]),
    ("Timebase, cumulative counters", [
        ("fast_iq_slew_ticks", "#e377c2", "fast_iq_slew_ticks [cumulative 16 kHz inner steps]"),
        ("fg_skips_total", "#1f77b4", "fg_skips_total [cumulative skipped 4 kHz ticks]"),
        ("fg_resume_count", "#8c564b", "fg_resume_count [cumulative foreground resumes]"),
    ]),
    ("PAS edge injection counters (what the generator did to the line)", [
        ("pas_edges_dropped", "#1f77b4", "pas_edges_dropped [cumulative missed edges]"),
        ("pas_edges_deferred", "#2ca02c", "pas_edges_deferred [cumulative late edges]"),
    ]),
    ("PAS sampler counters (what production made of it)", [
        ("pas_sampler_forward", "#2ca02c", "pas_sampler_forward [production sampler]"),
        ("pas_sampler_reverse", "#d62728", "pas_sampler_reverse [production sampler]"),
        ("pas_sampler_glitch", "#9467bd", "pas_sampler_glitch [production sampler]"),
        ("pas_sampler_invalid", "#ff7f0e", "pas_sampler_invalid [production sampler]"),
        ("pas_sampler_overflow", "#7f7f7f", "pas_sampler_overflow [production sampler]"),
    ]),
]

def esc(s: str) -> str:
    """XML-escape text placed inside SVG. The previous body replaced each character with
    ITSELF, so a label containing & or < produced malformed SVG."""
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))


def nice_ticks(lo: float, hi: float, n: int = 5) -> list[float]:
    import math as _math
    if hi <= lo:
        return [lo]
    raw = (hi - lo) / n
    if raw <= 0:
        return [lo, hi]
    mag = 10 ** _math.floor(_math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        step = m * mag
        if step >= raw - 1e-12:
            break
    start = (int(lo / step)) * step
    out, v = [], start
    while v <= hi + step * 0.5:
        if v >= lo - 1e-9:
            out.append(round(v, 9))
        v += step
    return out or [lo, hi]


def panel_svg(y0: int, title: str, series, cols, t0, t1, events) -> str:
    xs = cols["time_s"]
    ally = []
    for name, _, _ in series:
        ally += [v for t, v in zip(xs, cols[name]) if t0 <= t <= t1]
    lo, hi = (min(ally), max(ally)) if ally else (0.0, 1.0)
    if hi - lo < 1e-9:
        lo, hi = lo - 1, hi + 1
    pad = (hi - lo) * 0.08
    lo, hi = lo - pad, hi + pad

    px0, px1 = PAD_L, W - PAD_R
    py0, py1 = y0 + PAD_T, y0 + H - PAD_B

    def X(t):
        return px0 + (t - t0) / (t1 - t0) * (px1 - px0)

    def Y(v):
        return py1 - (v - lo) / (hi - lo) * (py1 - py0)

    p = [f'<text x="{PAD_L}" y="{y0 + 17}" class="ttl">{esc(title)}</text>',
         f'<rect x="{px0}" y="{py0}" width="{px1-px0}" height="{py1-py0}" class="frame"/>']

    for v in nice_ticks(lo, hi):
        y = Y(v)
        if py0 - 1 <= y <= py1 + 1:
            p.append(f'<line x1="{px0}" y1="{y:.1f}" x2="{px1}" y2="{y:.1f}" class="grid"/>')
            p.append(f'<text x="{px0-6}" y="{y+3.5:.1f}" class="ax" text-anchor="end">{v:g}</text>')
    for t in nice_ticks(t0, t1, 8):
        if t0 <= t <= t1:
            x = X(t)
            p.append(f'<line x1="{x:.1f}" y1="{py0}" x2="{x:.1f}" y2="{py1}" class="grid"/>')
            p.append(f'<text x="{x:.1f}" y="{py1+15}" class="ax" text-anchor="middle">{t:g}</text>')

    for i, (name, tt) in enumerate(events):
        x = X(tt)
        p.append(f'<line x1="{x:.1f}" y1="{py0}" x2="{x:.1f}" y2="{py1}" class="ev"/>')
        p.append(f'<text x="{x+3:.1f}" y="{py0+11+i*11}" class="evt">{esc(name)} {tt:g}s</text>')

    for i, (name, colour, label) in enumerate(series):
        pts = " ".join(f"{X(t):.1f},{Y(v):.1f}" for t, v in zip(xs, cols[name]) if t0 <= t <= t1)
        p.append(f'<polyline points="{pts}" fill="none" stroke="{colour}" stroke-width="1.1"/>')
        ly = py0 + 12 + i * 15
        p.append(f'<line x1="{px1+10}" y1="{ly-4}" x2="{px1+28}" y2="{ly-4}" '
                 f'stroke="{colour}" stroke-width="2"/>')
        p.append(f'<text x="{px1+LEGEND_X_OFFSET}" y="{ly}" class="lg">{esc(label)}</text>')
    return "\n".join(p)


SESSION_LEGEND = " | ".join(f"{v}={n}" for v, n in sorted(em.SESSION_NAMES.items()))
DEBUG_FLAG_LEGEND = " | ".join(f"0x{bit:02x}={n}" for bit, n in em.DEBUG_FLAG_BITS)

FOOTER_FS = 9.5
FOOTER_LINE_H = 13
FOOTER_CHAR_W = FOOTER_FS * 0.6


def wrap_text(s: str, width_px: float) -> list[str]:
    max_chars = max(20, int(width_px / FOOTER_CHAR_W))
    words, lines, cur = s.split(), [], ""
    for w in words:
        cand = f"{cur} {w}".strip()
        if len(cand) <= max_chars:
            cur = cand
        else:
            if cur:
                lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines or [""]


FOOTER_BLOCKS = [
    f"session: {SESSION_LEGEND}",
    f"debug_flags (bitmask, decoded bitwise): {DEBUG_FLAG_LEGEND}",
    ("time [s] · every exported sample drawn, no smoothing · forced-input harness: "
     "u_abs=0, no actual current / bridge / mechanical evidence · Iq chain is a request "
     "composition in Iq units, not Nm/A · torque_run_native is a native sensor count "
     "(~27/kg), not Nm"),
]


def make_figure(case_name: str, cols: dict, events, t0: float, t1: float,
                subtitle: str, out: Path) -> None:
    avail_w = W - 2 * PAD_L
    footer_lines = [ln for block in FOOTER_BLOCKS for ln in wrap_text(block, avail_w)]
    legend_h = 12 + FOOTER_LINE_H * len(footer_lines)
    total_h = 44 + H * len(PANELS) + legend_h
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{total_h}" '
             f'viewBox="0 0 {W} {total_h}">',
             '<style>'
             'text{font-family:ui-monospace,Consolas,monospace}'
             '.hd{font-size:15px;font-weight:600;fill:#111}'
             '.sub{font-size:11px;fill:#444}'
             '.ttl{font-size:12px;font-weight:600;fill:#222}'
             '.ax{font-size:10px;fill:#555}.lg{font-size:10px;fill:#222}'
             '.evt{font-size:9px;fill:#b00}'
             '.frame{fill:#fff;stroke:#bbb;stroke-width:1}'
             '.grid{stroke:#eee;stroke-width:1}'
             '.ev{stroke:#d62728;stroke-width:1;stroke-dasharray:4 3}'
             '.leg{font-size:9.5px;fill:#333}'
             '</style>',
             f'<rect width="{W}" height="{total_h}" fill="#fff"/>',
             f'<text x="{PAD_L}" y="20" class="hd">{esc(case_name)}</text>',
             f'<text x="{PAD_L}" y="36" class="sub">{esc(subtitle)}</text>']
    for i, (title, series) in enumerate(PANELS):
        parts.append(panel_svg(44 + i * H, title, series, cols, t0, t1, events))
    legend_y = 44 + H * len(PANELS)
    for i, line in enumerate(footer_lines):
        parts.append(f'<text x="{PAD_L}" y="{legend_y + 12 + i * FOOTER_LINE_H}" '
                     f'class="leg">{esc(line)}</text>')
    parts.append("</svg>")
    out.write_text("\n".join(parts), encoding="utf-8")


REQUIRED_PLOTS = [
    # Reverse
    ("reverse_clean_cad30_speed0", "clean reverse at 30 rpm, standing", None),
    ("reverse_clean_cad72_speed18", "clean reverse at 72 rpm, rolling 18 km/h", None),
    ("reverse_bounce_cad72_speed0", "reverse bounce (3-tick) at 72 rpm, standing", None),
    ("reverse_bounce_cad72_speed18", "reverse bounce (3-tick) at 72 rpm, rolling 18 km/h", None),
    # Sensor invalid
    ("tq_invalid_cad72_speed0", "torque sensor invalid at 72 rpm, standing", None),
    ("pas_invalid_cad72_speed18", "pas sensor invalid at 72 rpm, rolling", None),
    ("pas_invseq_cad72_speed0", "PAS invalid sequence at 72 rpm, standing", None),
    # Timebase
    ("miss_tick4_cad72_speed0", "miss_tick_every_n=4 at 72 rpm, standing", None),
    ("fg_delay4_cad72_speed18", "fg_delay=4 ticks at 72 rpm, rolling", (1.8, 2.6)),
    ("fg_delay20_cad72_speed0", "fg_delay=20 ticks at 72 rpm, standing", (1.8, 2.6)),
    # Regressions
    ("regr_linear72_baseline", "regression: linear72 baseline", None),
    ("regr_stop_release_cad72", "regression: stop/release at 72 rpm", None),
    # Control references (no disturbance key set at all)
    ("control_8s_cad72_speed18", "control: undisturbed 8 s ride, 72 rpm, rolling", None),
    ("control_6s_cad30_speed0", "control: undisturbed 6 s ride, 30 rpm, standing", None),
    # PAS edge (electrical) - missed edge and late edge
    ("pas_edge_drop3_cad72_speed18", "PAS missed edge (every 3rd) at 72 rpm, rolling", None),
    ("pas_edge_drop3_cad30_speed0", "PAS missed edge (every 3rd) at 30 rpm, standing", None),
    ("pas_edge_jitter8_cad72_speed18", "PAS late edge (8/4 ticks) at 72 rpm, rolling", None),
    ("pas_edge_jitter2_cad30_speed0", "PAS late edge (2/1 ticks) at 30 rpm, standing", None),
    # Timebase at 30 rpm and the full 4 kHz row rate
    ("miss_tick20_cad30_speed18", "miss_tick_every_n=20 at 30 rpm, rolling", None),
    ("fg_delay1_cad30_speed0", "fg_delay=1 tick at 30 rpm, standing", (1.9, 2.2)),
    ("hires_fg_delay1_cad72_speed18", "fg_delay=1 tick, FULL 4 kHz row rate", (1.1995, 1.2015)),
    ("hires_fg_delay4_cad72_speed18", "fg_delay=4 ticks, FULL 4 kHz row rate", (1.1995, 1.2025)),
    ("hires_fg_delay20_cad72_speed18", "fg_delay=20 ticks, FULL 4 kHz row rate", (1.1990, 1.2070)),
    # Sensor invalid at the other cadence/speed corner
    ("tq_invalid_cad30_speed18", "torque sensor invalid at 30 rpm, rolling", None),
    ("pas_invalid_cad30_speed0", "pas sensor invalid at 30 rpm, standing", None),
    # The bounce lasts 3 ticks = 0.75 ms. Only a full-4 kHz run can show it, and only a zoom can
    # make it readable; the 8 s sample_ms=1 figures cannot resolve it at all (D011-04).
    ("hires_bounce_cad72_speed18", "3-tick PAS bounce, FULL 4 kHz row rate, 72 rpm rolling",
     (1.1990, 1.2030)),
    ("hires_bounce_cad30_speed0", "3-tick PAS bounce, FULL 4 kHz row rate, 30 rpm standing",
     (1.1990, 1.2030)),
    ("hires_control_cad72_speed18", "control for the full-rate bounce, 72 rpm rolling", None),
    # P-1 evidence: the command, the first reverse LINE edge and the inhibit are three different
    # instants. Only a full-4 kHz record resolves them (D012-01).
    ("hires_reverse_cad72_speed18",
     "clean reverse, FULL 4 kHz: command 1.0 s, first reverse line edge 1.005 s, inhibit same tick",
     (0.9990, 1.0300)),
    ("hires_reverse_cad30_speed0",
     "clean reverse, FULL 4 kHz at 30 rpm: first reverse line edge 20.75 ms after the command",
     (0.9990, 1.0400)),
]


POLISH_INDEX = PLOTS / "index.pl.md"


# Every plot must land in exactly one section of the Polish index. The section is chosen by the
# FIRST matching rule, and a plot that matches none is a hard error - the previous version filtered
# by ad-hoc substrings, so the control and PAS-edge figures were rendered but never listed.
INDEX_SECTIONS = [
    ("Przypadki kontrolne (bez zadnego zaklocenia)",
     lambda n: n.startswith("control_"),
     lambda n: "odniesienie: ta sama jazda bez zaklocenia"),
    ("Cofanie pedalow (reverse)",
     lambda n: n.startswith("reverse_"),
     lambda n: "czyste cofanie" if "clean" in n else "pojedyncze krotkie odbicie (3 ticki)"),
    ("Cofanie w pelnej rozdzielczosci 4 kHz - dowod do P-1 (komenda / krawedz / inhibit)",
     lambda n: n.startswith("hires_reverse"),
     lambda n: "komenda, pierwsza odwrotna krawedz linii i inhibit to trzy rozne chwile"),
    ("Odbicie linii PAS w pelnej rozdzielczosci 4 kHz (3 ticki = 0,75 ms)",
     lambda n: n.startswith("hires_bounce") or n.startswith("hires_control"),
     lambda n: ("kontrola dla przebiegu 4 kHz" if n.startswith("hires_control") else
                "pojedyncze odbicie elektryczne; korba caly czas jedzie do przodu")),
    ("Niewaznosc czujnikow (sensor invalid)",
     lambda n: n.startswith(("tq_invalid", "pas_invalid", "pas_invseq")),
     lambda n: ("nieprawidlowosc momentu" if n.startswith("tq_invalid") else
                ("nieprawidlowa sekwencja PAS" if n.startswith("pas_invseq") else
                 "nieprawidlowosc PAS"))),
    ("Zaklocenie krawedzi PAS (elektryczne: zgubiona / spozniona krawedz)",
     lambda n: n.startswith("pas_edge_"),
     lambda n: ("zgubiona krawedz - do ISR nie dociera co N-ta zmiana" if "drop" in n else
                "spozniona krawedz - liczba zmian zachowana, zmienia sie tylko ich czas")),
    ("Zaklocenia czasu (timebase: opozniona obsluga foreground)",
     lambda n: n.startswith(("miss_tick", "fg_delay", "hires_fg_delay")),
     lambda n: ("pomijanie tickow w oknie" if n.startswith("miss_tick") else
                ("zastoj foreground, PELNA rozdzielczosc 4 kHz" if n.startswith("hires_") else
                 "zastoj foreground"))),
    ("Regresje (nowe opcje niewlaczone - stare kolumny musza byc identyczne)",
     lambda n: n.startswith("regr_"),
     lambda n: "regresja wobec przyjetej serii"),
]


def write_polish_index(written_by_case: dict[str, list[str]]) -> None:
    lines = [
        "# AP-01 disturbances-001 - indeks wykresow",
        "",
        "Krotkie wyjasnienie dla czytelnika spoza firmware, po polsku. Kazdy wykres to plik SVG - "
        "otworz go w przegladarce.",
        "",
        "## Co to jest 'zadanie Iq' (Iq ref / iq_ref itd.)",
        "",
        "`Iq` to wewnetrzna wielkosc regulatora silnika (skladowa pradu w osi q wektorowego "
        "sterowania FOC) uzywana jako 'ile wspomagania silnik ma teraz dostarczyc'. Kanaly "
        "`iq_before_profile_limit -> iq_mode_request -> iq_requested -> iq_allowed -> iq_ref` to "
        "KOLEJNE etapy skladania tego zadania (limity profilu, tryb, dopuszczenie, ostateczna "
        "wartosc przy 16 kHz) - **nie** sa to Nm (moment) ani A (prad rzeczywisty silnika). Ten "
        "harness (Controller Lab) nie zamyka petli PMSM/FOC: `u_abs` i zmierzony prad baterii sa "
        "stale zerem, wiec te wykresy pokazuja WYLACZNIE kompozycje zadania, nie rzeczywiste "
        "zachowanie elektryczne/mechaniczne.",
        "",
        "## Jak czytac te wykresy",
        "",
        "Panele sa pogrupowane WEDLUG SKALI, nie tematu: flaga 0/1 i licznik kumulacyjny rzedu "
        "20 000 nie maja wspolnej osi Y, wiec narysowane razem nic nie pokazuja. Pionowe czerwone "
        "linie przerywane to czasy ZADANE (wejscie); reakcja produkcji jest mierzona osobno w "
        "`results/disturbances-metrics.json` i zwykle nastepuje pozniej. Rysowana jest KAZDA "
        "probka z zakresu - bez wygladzania, bez decymacji, bez usredniania.",
        "",
        "Kazda kategoria ma przypadek kontrolny (bez zaklocenia), wstrzykniecie w trakcie "
        "ustalonej jazdy i powrot wejscia do normy przed koncem przebiegu.",
        "",
    ]
    listed: set[str] = set()
    for title, match, describe in INDEX_SECTIONS:
        section = [(n, sub) for n, sub, _z in REQUIRED_PLOTS if match(n)]
        if not section:
            continue
        lines += [f"## {title}", ""]
        for name, subtitle in section:
            files = written_by_case.get(name, [])
            listed.update(files)
            links = " , ".join(f"[{f}]({f})" for f in files)
            lines.append(f"- **{name}** ({describe(name)}): {esc(subtitle)} -> {links}")
        lines.append("")

    unlisted = sorted({f for fs in written_by_case.values() for f in fs} - listed)
    if unlisted:
        raise SystemExit(f"plots rendered but not listed in the Polish index: {unlisted}")

    lines += [
        "## Legenda stanow (na kazdym wykresie, na dole)",
        "",
        f"- session: {SESSION_LEGEND}",
        f"- debug_flags (maska bitowa): {DEBUG_FLAG_LEGEND}",
        "- pas_direction_state: 0=FORWARD_SAFE, 1=DIRECTION_INHIBIT, 2=FORWARD_CONFIRMING",
        "- pas_inhibit_reason: 0=brak, 1=cofanie, 2=nieprawidlowa sekwencja",
        "- ride_interval: 0=nie pedaluje, 1=pierwszy odcinek jazdy, 2=po restarcie",
        "- crank_direction: -1=cofanie, 0=brak ruchu, 1=do przodu",
        "",
        "## Ograniczenia dowodowe",
        "",
        "Ten harness NIE zamyka petli PMSM/FOC (u_abs=0, brak realnego pradu baterii). Kadencja/"
        "moment/predkosc to wymuszone wejscia syntetyczne. Wykresy pokazuja kompozycje zadania "
        "(request), nie osiagi mechaniczne/elektryczne ani zachowanie sprzetu. Zadna dlugosc "
        "zastoju (1/4/20 tickow) nie jest limitem bezpiecznym dla sprzetu - to punkty cwiczenia "
        "API. `motor_erps` jest w harnessie wyprowadzone z kadencji i jest BEZ ZNAKU, wiec proby "
        "cofania nie dotykaja obslugi kierunku po stronie silnika.",
        "",
        "Proponowane (niezatwierdzone) kryteria: [../PROPOSED_CRITERIA.md](../PROPOSED_CRITERIA.md).",
        "",
    ]
    POLISH_INDEX.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    import json
    manifest = json.loads((RESULTS / "disturbances-results.json").read_text(encoding="utf-8"))
    by_name = {c["name"]: c for c in manifest["cases"]}
    written = []
    written_by_case: dict[str, list[str]] = {}
    missing: list[str] = []
    for name, subtitle, zoom in REQUIRED_PLOTS:
        if name not in by_name:
            missing.append(name)
            continue
        c = by_name[name]
        cols = em.read_csv(RESULTS / c["csv_file"])
        events = sorted(c["commanded_event_times_s"].items(), key=lambda kv: kv[1])
        t0, t1 = (zoom if zoom else (cols["time_s"][0], cols["time_s"][-1]))
        args = c["effective_args"]
        sub = (f"{subtitle} | {'zoom ' if zoom else 'full run '}"
               f"[{t0:g}, {t1:g}] s | csv sha256 {c['csv_sha256'][:16]}"
               f" | cadence={args['cadence']} rpm speed={args['speed']} km/h "
               f"torque={args['torque']} Nm ripple={args['torque_ripple']}% asym={args['asymmetry']}%")
        out = PLOTS / f"{name}{'_zoom' if zoom else ''}.svg"
        make_figure(name, cols, events, t0, t1, sub, out)
        written.append(out.name)
        written_by_case.setdefault(name, []).append(out.name)
        if zoom:
            out2 = PLOTS / f"{name}_full.svg"
            sub2 = sub.replace(f"zoom [{t0:g}, {t1:g}] s", "full run")
            make_figure(name, cols, events, cols["time_s"][0], cols["time_s"][-1], sub2, out2)
            written.append(out2.name)
            written_by_case[name].append(out2.name)
    if missing:
        # A required category without a plot is an incomplete deliverable, not a note.
        print(f"MISSING from the manifest, no plot produced: {missing}")
        return 1
    write_polish_index(written_by_case)
    written.append(POLISH_INDEX.name)
    print(f"wrote {len(written)} files to plots/ (incl. {POLISH_INDEX.name}):")
    for w in written:
        print("  ", w)
    return 0


if __name__ == "__main__":
    sys.exit(main())