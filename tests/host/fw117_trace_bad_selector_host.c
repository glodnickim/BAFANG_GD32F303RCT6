/*
 * FW-117.1: this file exists only to prove FW117_TRACE_TRIGGER_EVENT rejects an out-of-range
 * value at COMPILE time, not at runtime - the property cannot be expressed as a CHECK() in a
 * program that has to link and run to report anything.
 *
 * The matching suite entry in run-host-tests.ps1 builds this file with
 * -DFW117_TRACE_TRIGGER_EVENT=2 and (via ExpectBuildFailure = $true) asserts the BUILD fails.
 * If FW117_TRACE_TRIGGER_EVENT ever stops being validated, this file starts compiling instead -
 * the suite then fails for the opposite reason (a build that unexpectedly succeeded), so the
 * regression is still caught either way.
 */
#include "fw117_trace.h"

int fw117_trace_bad_selector_probe(void) { return 0; }
