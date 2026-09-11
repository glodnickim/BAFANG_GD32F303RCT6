"""AP-01 disturbances-001: measurement adapter for records with disturbance events.

Re-exports the ACCEPTED event/window primitives from
../transients-001/event_metrics.py. They are loaded BY PATH (accepted_modules.py) and this module
does not touch sys.path: AP-01, transients-001 and disturbances-001 each ship a package called
`scenarios`, so a sys.path insert here silently redirected `import scenarios` in every module that
imported this one (REVIEW-EVD-AP-01-010, D010-05).

No production maths and no accepted primitive is redefined here.
"""
from __future__ import annotations

from accepted_modules import accepted_event_metrics

_em = accepted_event_metrics()

event_windows = _em.event_windows
window_stats = _em.window_stats
step_response = _em.step_response
stop_between = _em.stop_between
first_crossing = _em.first_crossing
restart_analysis = _em.restart_analysis
excursions = _em.excursions
commanded_input_check = _em.commanded_input_check
decode_session = _em.decode_session
decode_debug_flags = _em.decode_debug_flags
read_csv = _em.read_csv
DEFAULT_ZERO_TOLERANCE = _em.DEFAULT_ZERO_TOLERANCE
DEFAULT_MIN_CONFIRM_S = _em.DEFAULT_MIN_CONFIRM_S
DEFAULT_MAX_TRANSITION_GAP_S = _em.DEFAULT_MAX_TRANSITION_GAP_S
SESSION_NAMES = _em.SESSION_NAMES
DEBUG_FLAG_BITS = _em.DEBUG_FLAG_BITS

SOURCE_FILE = _em.__file__
