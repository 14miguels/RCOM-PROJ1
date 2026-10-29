#!/usr/bin/env bash
# RCOM 2026/2027 - randomized stress test
#
# Many transfers through build/relay in random mode (frames dropped, corrupted in
# ANY byte, duplicated). The invariant checked is the important one:
#   a transfer reported as completed MUST have the same SHA-256 as the original,
#   otherwise the protocol must report FAILED and leave no output file.
# Transfers may legitimately FAIL when the retry budget is exhausted; they are counted.
#
#   ./tests/run_stress.sh [runs] [drop%] [corrupt%] [dup%]     (defaults: 20 5 5 3)
cd "$(dirname "$0")" || exit 1
B=$PWD/build
DRV=${DRV:-$B/driver_san}
RUNS=${1:-20}; DROP=${2:-5}; CORR=${3:-5}; DUP=${4:-3}
W=$(mktemp -d "${TMPDIR:-/tmp}/rcom-st.XXXXXX") || exit 1
P=$(mktemp -d /tmp/rcom-p.XXXXXX) || exit 1
trap 'pkill -P $$ 2>/dev/null; rm -rf "${W:?}" "${P:?}"' EXIT
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

python3 - "$W" <<'PY'
import sys, os
d = sys.argv[1]
open(d + "/rand.bin", "wb").write(os.urandom(30000))
open(d + "/special.bin", "wb").write(b"\x7e\x7d\x5e\x5d\x00" * 6000)
PY
ok=0; failed=0; bad=0
for i in $(seq 1 "$RUNS"); do
    f=$W/rand.bin; [ $((i % 2)) = 0 ] && f=$W/special.bin
    rm -f "$W/out.bin" "$W/out.bin.part"
    RELAY_CHAOS="$i:$DROP:$CORR:$DUP" "$B/relay" "$P/t" "$P/r" 2> "$W/relay.log" & relay=$!
    for _ in $(seq 50); do [ -e "$P/t" ] && [ -e "$P/r" ] && break; sleep 0.1; done
    "$DRV" "$P/r" 9600 rx "$W/out.bin" 8 1 > "$W/rx.log" 2>&1 & rx=$!
    sleep 0.3
    "$DRV" "$P/t" 9600 tx "$f" 8 1 > "$W/tx.log" 2>&1 & tx=$!
    t=0; while { kill -0 $tx 2>/dev/null || kill -0 $rx 2>/dev/null; } && [ $t -lt 900 ]; do sleep 0.1; t=$((t+1)); done
    hung=0; if kill -0 $tx 2>/dev/null || kill -0 $rx 2>/dev/null; then hung=1; kill -9 $tx $rx 2>/dev/null; fi
    kill $relay 2>/dev/null; wait $relay 2>/dev/null
    faults=$(grep -c chaos "$W/relay.log")
    verdict=
    if [ $hung = 1 ]; then verdict="HANG"
    elif grep -qE "AddressSanitizer|runtime error" "$W"/*.log; then verdict="SANITIZER"
    elif grep -q "received (" "$W/rx.log"; then
        [ -f "$W/out.bin" ] && [ "$(sha "$f")" = "$(sha "$W/out.bin")" ] || verdict="WRONG-FILE-REPORTED-AS-OK"
    else
        { [ -e "$W/out.bin" ] || [ -e "$W/out.bin.part" ]; } && verdict="FAILED-BUT-LEFT-FILE"
    fi
    if [ -n "$verdict" ]; then bad=$((bad+1)); echo "run $i: BAD ($verdict) seed=$i faults=$faults"; cp -r "$W" "/tmp/rcom-stress-fail-$i"
    elif grep -q "received (" "$W/rx.log"; then ok=$((ok+1)); echo "run $i: ok, identical (injected faults: $faults, ${t}x0.1s)"
    else failed=$((failed+1)); echo "run $i: reported FAILED cleanly (retry budget exhausted; faults: $faults)"; fi
done
echo "stress: $ok identical, $failed failed cleanly, $bad INVARIANT VIOLATIONS (runs=$RUNS drop=$DROP% corrupt=$CORR% dup=$DUP%)"
[ $bad = 0 ]
