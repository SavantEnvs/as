#!/usr/bin/env bash
#
# mayhem/test.sh — RUN the known-answer self test (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Runs the known-answer self tests that mayhem/build.sh built (upstream ships no unit tests):
# srec/ihex parsing results, the CAN-TP / LIN-TP frames and the DoIP / SOME/IP / SD / UDS / XCP / CanNm messages the stacks send back, ODX-F flash blocks.
EXPECTED=47   # selftest.c 7, _lin 5, _doip 5, _someip 4, _dcm 6, _xcp 9, _cannm 8, _odx 3
passed=0; failed=0
for runner in "$SRC/mayhem/out/selftest" "$SRC/mayhem/out/selftest_lin" "$SRC/mayhem/out/selftest_doip" \
              "$SRC/mayhem/out/selftest_someip" "$SRC/mayhem/out/selftest_dcm" "$SRC/mayhem/out/selftest_xcp" \
              "$SRC/mayhem/out/selftest_cannm" "$SRC/mayhem/out/selftest_odx"; do
  if [ ! -x "$runner" ]; then
    echo "missing $runner (mayhem/build.sh should have built it)" >&2
    failed=$((failed + 1)); continue
  fi
  out="$(TMPDIR="${TMPDIR:-/tmp}" "$runner" 2>&1)"; rc=$?
  echo "$out"
  p=$(grep -c '^PASS ' <<<"$out" || true)
  f=$(grep -c '^FAIL ' <<<"$out" || true)
  # each runner must finish and report; a crash or a missing RESULT line is a failure
  grep -q '^RESULT passed=' <<<"$out" || f=$((f + 1))
  [ "$rc" -eq 0 ] || [ "$f" -gt 0 ] || f=1
  passed=$((passed + p)); failed=$((failed + f))
done
[ "$passed" -eq "$EXPECTED" ] || [ "$failed" -gt 0 ] || failed=$(( EXPECTED - passed ))
emit_ctrf "as-selftest" "$passed" "$failed" 0
