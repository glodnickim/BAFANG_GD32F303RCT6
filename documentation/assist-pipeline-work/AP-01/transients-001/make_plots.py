"""AP-01 transients-001: whole-path plots with the events marked.

Self-contained SVG, no plotting library (matplotlib is not installed in this environment and a
measurement artefact should not depend on one). Every sample that lands in the plotted time range
is drawn: NO smoothing, NO decimation, NO averaging. A plot that hid a single-sample cut to zero -
which is exactly what these records contain - would misrepresent the result.

One figure per case, stacked panels covering the WHOLE path:

  1  generator input      torque_gen_nm, torque_cmd_mean_nm, cadence_gen_rpm
  2  conditioned rider    torque_run_native, cadence_control_rpm
  3  Iq chain             iq_before_profile_limit, iq_mode_request, iq_requested, iq_allowed, iq_ref
  4  supervisory state    session, gate_steps, debug_flags, ride_interval

Event times are drawn as labelled vertical rules. They are the COMMANDED input times; the observed
reaction is in transients-metrics.json and is generally later.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import event_metrics as em  # noqa: E402

RESULTS = HERE / "results"
PLOTS = HERE / "plots"
PLOTS.mkdir(exist_ok=True)

W, H = 1320, 250          # panel width/height in px
PAD_L, PAD_R, PAD_T, PAD_B = 78, 300, 26, 34

# Series-legend geometry. The legend is drawn to the RIGHT of each panel starting at
# (W - PAD_R) + LEGEND_X_OFFSET, so a label may be at most LEGEND_MAX_CHARS wide before it is
# clipped by the image edge. Making this explicit is what turns "the legend was cut off" into a
# checkable property - test_transients.test_series_legend_labels_fit() asserts it for every label.
LEGEND_X_OFFSET = 33
LEGEND_FS = 10
LEGEND_CHAR_W = LEGEND_FS * 0.6
LEGEND_MAX_CHARS = int((PAD_R - LEGEND_X_OFFSET) / LEGEND_CHAR_W)

# Labels carry an explicit unit so Nm/rpm/counts never masquerade as one physical unit
# (REVIEW-EVD-AP-01-005, T005-03). "Iq counts" are the FOC current-loop request in the firmware's
# internal Iq units (a request composition, per the qualification note) - NOT amps and NOT a torque
# or speed unit, hence spelled out rather than left bare.
PANELS = [
    ("Generator input (synthetic rider)", [
        ("torque_gen_nm", "#1f77b4", "torque_gen_nm [Nm]"),
        ("torque_cmd_mean_nm", "#d62728", "torque_cmd_mean_nm [Nm] (commanded mean)"),
        ("cadence_gen_rpm", "#2ca02c", "cadence_gen_rpm [rpm]"),
    ]),
    ("Conditioned rider input (production; torque is in native sensor counts, not Nm)", [
        # REVIEW-EVD-AP-01-006 (T006-03): this channel is ts->assist_delta_run_native
        # (controller_lab.c line 486), a uint16 native torque-sensor delta - inc/torque_input.h
        # line 194, whose own comment scales it as "27 native ~ 1 kg". It is NOT Nm. Only the label
        # changes here; the plotted data is untouched, and converting to Nm would need an explicit
        # calibration this rework does not require.
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
    ("Supervisory state (see legend below for session/debug_flags decode)", [
        ("session", "#1f77b4", "session [enum 0-3]"),
        ("gate_steps", "#2ca02c", "gate_steps [counts]"),
        ("debug_flags", "#ff7f0e", "debug_flags [bitmask]"),
        ("ride_interval", "#7f7f7f", "ride_interval [0/1/2] (generator)"),
    ]),
]


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def nice_ticks(lo: float, hi: float, n: int = 5) -> list[float]:
    """Standard 1-2-5 tick stepping, picking `mag` from the ACTUAL requested range/step.

    REVIEW-EVD-AP-01-005 (T005-03): the previous version hardcoded `mag = 10**-3` for any raw step
    below 1.0, regardless of how far below - so a 4.6-6.2 s zoomed restart panel wanting an ~0.2 s
    tick step (raw = 1.6/8 = 0.2) was forced down to a ~0.01 s grid, producing labels that overlap
    across the whole panel width. `mag` must instead be the power of ten of `raw` itself, found with
    log10, whether raw is above or below 1 - the same 1-2-5 selection then works uniformly.
    """
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


# Session/debug_flags legend text, decoded from inc/ride_session.h / inc/ride_control.h (not
# guessed - REVIEW-EVD-AP-01-005, T005-01/T005-03). Built from event_metrics's single source of
# truth so the plot legend and the analysis JSON can never disagree.
SESSION_LEGEND = " | ".join(f"{v}={n}" for v, n in sorted(em.SESSION_NAMES.items()))
DEBUG_FLAG_LEGEND = " | ".join(f"0x{bit:02x}={n}" for bit, n in em.DEBUG_FLAG_BITS)

FOOTER_FS = 9.5           # px, monospace
FOOTER_LINE_H = 13        # px between wrapped footer lines
FOOTER_CHAR_W = FOOTER_FS * 0.6   # monospace advance for ui-monospace/Consolas


def wrap_text(s: str, width_px: float) -> list[str]:
    """Greedy word wrap for the footer, in characters that fit `width_px` at FOOTER_CHAR_W.

    REVIEW-EVD-AP-01-006 (T006-03): the bitmask legend and the limitations note were drawn one
    pixel apart (legend_y+27 vs total_h-6 with LEGEND_H=34) and both ran off the right edge - the
    debug_flags legend alone estimates ~1460 px against a 1320 px image. Wrapping plus a footer
    height computed from the number of wrapped lines fixes both; nothing about the plotted data
    changes.
    """
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
    # Footer height follows from the wrapped text, so lines can never be drawn on top of each
    # other or past the bottom edge (T006-03).
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


# Cases the card names explicitly: the step, the drop, the fast restart and the rolling restart.
REQUIRED_PLOTS = [
    ("step_up_ripple_cad72", "torque step 15 -> 40 Nm at 72 rpm, baseline ripple/asymmetry", None),
    ("step_down_ripple_cad72", "torque drop 40 -> 15 Nm at 72 rpm, baseline ripple/asymmetry", None),
    ("step_up_clean_cad72", "torque step 15 -> 40 Nm at 72 rpm, no ripple/asymmetry", None),
    ("step_down_clean_cad72", "torque drop 40 -> 15 Nm at 72 rpm, no ripple/asymmetry", None),
    ("step_up_clean_cad30", "torque step 15 -> 40 Nm at 30 rpm, no ripple/asymmetry", None),
    ("step_up_clean_cad120", "torque step 15 -> 40 Nm at 120 rpm, no ripple/asymmetry", None),
    # Subtitles corrected per REVIEW-EVD-AP-01-006 (T006-01): they previously asserted "restart
    # during the Iq decay" and "at the stop-declaration instant". A nonzero sample is not evidence
    # of a decay, and nothing here establishes a declared stop. Each subtitle now states only the
    # scripted gap and what the export actually shows; the verdict lives in
    # rework-002/transients-metrics-rework-002.json restart_classification.
    ("restart_gap0195ms_speed0",
     "195 ms scripted gap, standing (speed 0) - Iq still nonzero at the last pre-restart sample",
     (4.6, 6.2)),
    ("restart_gap0195ms_speed18",
     "195 ms scripted gap, ROLLING (18 km/h) - Iq still nonzero at the last pre-restart sample",
     (4.6, 6.2)),
    ("restart_gap0200ms_speed0",
     "200 ms scripted gap, standing - zero-crossing bracket contains the restart (order "
     "undetermined)", (4.6, 6.2)),
    ("restart_gap0200ms_speed18",
     "200 ms scripted gap, ROLLING - zero-crossing bracket contains the restart (order "
     "undetermined)", (4.6, 6.2)),
    ("restart_gap1500ms_speed0",
     "control case: 1500 ms gap, standing - Iq observed at zero before the restart", None),
    ("restart_gap1500ms_speed18",
     "control case: 1500 ms gap, ROLLING - Iq observed at zero before the restart", None),
    ("restart_then_stop2_speed18", "stop, restart, second stop (ride_stop2_s), rolling", None),
]


POLISH_INDEX = PLOTS / "index.pl.md"


def write_polish_index(written_by_case: dict[str, list[str]]) -> None:
    """Simple Polish-language index (REVIEW-EVD-AP-01-005, T005-03) linking skok/spadek/restart
    charts, for a reader who is not a firmware engineer."""
    lines = [
        "# AP-01 transients-001 - indeks wykresow",
        "",
        "Krotkie wyjasnienie dla czytelnika spoza firmware, po polsku. Kazdy wykres to plik SVG - "
        "otworz go w przegladarce.",
        "",
        "## Co to jest 'zadanie Iq' (Iq ref / iq_ref itd.)",
        "",
        "`Iq` to wewnetrzna wielkosc regulatora silnika (skladowa prądu w osi q wektorowego "
        "sterowania FOC) uzywana jako 'ile wspomagania silnik ma teraz dostarczyc'. Kanaly "
        "`iq_before_profile_limit -> iq_mode_request -> iq_requested -> iq_allowed -> iq_ref` to "
        "KOLEJNE etapy skladania tego zadania (limity profilu, tryb, dopuszczenie, ostateczna "
        "wartosc przy 16 kHz) - **nie** sa to Nm (moment) ani A (prad rzeczywisty silnika). Ten "
        "harness (Controller Lab) nie zamyka petli PMSM/FOC: `u_abs` i zmierzony prad baterii sa "
        "stale zerem, wiec te wykresy pokazuja WYLACZNIE kompozycje zadania, nie rzeczywiste "
        "zachowanie elektryczne/mechaniczne.",
        "",
        "## Skok momentu (torque step)",
        "",
    ]
    for name, subtitle, zoom in REQUIRED_PLOTS:
        if "step" not in name:
            continue
        files = written_by_case.get(name, [])
        links = " , ".join(f"[{f}]({f})" for f in files)
        pl = "wzrost momentu" if name.startswith("step_up") else "spadek momentu"
        lines.append(f"- **{name}** ({pl}): {esc(subtitle)} -> {links}")
    lines += ["", "## Restart (stop i ponowne pedalowanie)", ""]
    for name, subtitle, zoom in REQUIRED_PLOTS:
        if "restart" not in name:
            continue
        files = written_by_case.get(name, [])
        links = " , ".join(f"[{f}]({f})" for f in files)
        lines.append(f"- **{name}**: {esc(subtitle)} -> {links}")
    lines += [
        "",
        "## Jak czytac opis restartu",
        "",
        "Kolejnosc zdarzen rozstrzyga PRZEDZIAL PRZEKROCZENIA ZERA - odcinek miedzy ostatnia "
        "probka niezerowa a pierwsza zerowa - porownany z zadana chwila restartu:",
        "",
        "- **zero zaobserwowane przed restartem**: kolejnosc jest ZNANA. Niepewna zostaje tylko "
        "dokladna chwila przekroczenia wewnatrz przedzialu i dlugosc potwierdzenia.",
        "- **kolejnosc nierozstrzygnieta**: przedzial ZAWIERA chwile restartu, wiec eksport (co "
        "5 ms) nie mowi, co bylo pierwsze. Tak jest w probie 200 ms.",
        "- **Iq niezerowe przy ostatniej probce przed restartem**: to tylko stwierdzenie faktu. "
        "Pojedyncza niezerowa probka NIE dowodzi, ze wspomaganie wlasnie opadalo ani ze trwal "
        "release. Tak jest w probie 195 ms.",
        "",
        "Zero przed restartem to **nie to samo** co potwierdzony stop - stop jest liczony osobno.",
        "",
        "## Legenda stanow (na kazdym wykresie, na dole)",
        "",
        f"- session: {SESSION_LEGEND}",
        f"- debug_flags (maska bitowa): {DEBUG_FLAG_LEGEND}",
        "",
        "## Ograniczenia dowodowe",
        "",
        "Ten harness NIE zamyka petli PMSM/FOC (u_abs=0, brak realnego pradu baterii). Kadencja/"
        "moment/predkosc to wymuszone wejscia syntetyczne. Wykresy pokazuja kompozycje zadania "
        "(request), nie osiagi mechaniczne/elektryczne ani zachowanie sprzetu.",
        "",
        "Zrodlo: [REVIEW-EVD-AP-01-006](../../../../../../integration/task-reports/"
        "REVIEW-EVD-AP-01-006.md), poprawka w "
        "[EXEC-EVD-AP-01-007](../../../../../../integration/task-reports/EXEC-EVD-AP-01-007.md). "
        "Poprawione metryki: `rework-002/transients-metrics-rework-002.json`.",
    ]
    POLISH_INDEX.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    manifest = json.loads((RESULTS / "transients-results.json").read_text(encoding="utf-8"))
    by_name = {c["name"]: c for c in manifest["cases"]}
    written = []
    written_by_case: dict[str, list[str]] = {}
    for name, subtitle, zoom in REQUIRED_PLOTS:
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
        # Full-run companion for the zoomed restart figures, so the whole path is always available.
        if zoom:
            out2 = PLOTS / f"{name}_full.svg"
            sub2 = sub.replace(f"zoom [{t0:g}, {t1:g}] s", "full run")
            make_figure(name, cols, events, cols["time_s"][0], cols["time_s"][-1], sub2, out2)
            written.append(out2.name)
            written_by_case[name].append(out2.name)
    write_polish_index(written_by_case)
    written.append(POLISH_INDEX.name)
    print(f"wrote {len(written)} files to plots/ (incl. {POLISH_INDEX.name}):")
    for w in written:
        print("  ", w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
