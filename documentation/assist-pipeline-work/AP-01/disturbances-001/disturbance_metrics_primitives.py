"""AP-01 disturbances-001 metrics primitives.

Re-exports the ACCEPTED AP-01 metrics primitives from ../metrics.py, loaded BY PATH so that no
sys.path entry is added (see disturbance_metrics.py for why that matters here).
No production maths is redefined here.
"""
from __future__ import annotations

from accepted_modules import accepted_metrics

_m = accepted_metrics()

mean = _m.mean
peak_to_peak = _m.peak_to_peak
rms_about_mean = _m.rms_about_mean
relative_ripple = _m.relative_ripple
window = _m.window
stop_points = _m.stop_points
rise_time_10_90_detail = _m.rise_time_10_90_detail
stage_analysis = _m.stage_analysis
limit_assessment = _m.limit_assessment

SOURCE_FILE = _m.__file__
