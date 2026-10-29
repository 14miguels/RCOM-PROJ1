#!/usr/bin/env bash
# RCOM 2026/2027 - integration tests
#
# Runs two INDEPENDENT processes (Tx and Rx, ASan/UBSan build) connected by
# build/relay (pseudoterminals + deterministic fault injection) and checks the
# received file with SHA-256. Run with:  make -C tests integration
# Needs: bash, python3 (test data), sha256sum or shasum.

cd "$(dirname "$0")" || exit 1
B=$PWD/build
DRV=${DRV:-$B/driver_san}
W=$(mktemp -d "${TMPDIR:-/tmp}/rcom-it.XXXXXX") || exit 1
# LinkLayer.serialPort holds only 50 characters, so the pty links need SHORT paths.
P=$(mktemp -d /tmp/rcom-p.XXXXXX) || exit 1
PASS=0; FAIL=0; FAILED_NAMES=""; KEEPW=""
cleanup() { pkill -P $$ 2>/dev/null; [ -z "$KEEPW" ] && rm -rf "${W:?}" "${P:?}"; }
trap cleanup EXIT

sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# ---------------------------------------------------------------- test data
D=$W/data
mkdir -p "$D"
printf 'The quick brown fox jumps over the lazy dog.\nRCOM 2026/2027\n' > "$D/text.txt"
head -c 50000 /dev/urandom > "$D/binary.bin"
: > "$D/empty.bin"
printf 'tiny' > "$D/small.bin"
head -c 1048576 /dev/urandom > "$D/large.bin"
python3 - "$D" <<'PY'
import sys, os
d = sys.argv[1]
open(d + "/zeros.bin", "wb").write(b"\x00" * 20000)
open(d + "/flags.bin", "wb").write(b"\x7e" * 20000)
open(d + "/esc.bin", "wb").write(b"\x7d" * 20000)
open(d + "/mixed.bin", "wb").write(b"\x7e\x7d\x5e\x5d\x00\x7d\x5e\x7e\x7e\x7d\x7d" * 3000)
open(d + "/random200k.bin", "wb").write(os.urandom(200000))
open(d + "/nearmax.bin", "wb").write((b"\x7e" * 996 + b"\x7d" * 4) * 30)  # DATA packets at the frame size limit
PY
cp ../penguin.gif "$D/penguin.gif"
head -c 30000 /dev/urandom > "$D/f30k.bin"

# ---------------------------------------------------------------- runner
# run_case NAME FILE [RELAY RULES...]
# env: TRIES (3) TIMEOUT (1) EXPECT (ok|fail) MAXWAIT (60)
#      TXGREP/RXGREP/RELAYGREP: extended regexes that must appear in that log
#      RXOUT: Rx output path (default: inside the case directory)
run_case() {
    local name=$1 file=$2; shift 2
    local tries=${TRIES:-3} to=${TIMEOUT:-1} expect=${EXPECT:-ok} maxwait=${MAXWAIT:-60}
    local c=$W/$name
    mkdir -p "$c"
    local out=${RXOUT:-$c/out.bin}
    local ok=1 why=""

    "$B/relay" "$P/${c##*/}.tx" "$P/${c##*/}.rx" "$@" 2> "$c/relay.log" & local relay=$!
    for _ in $(seq 50); do [ -e "$P/${c##*/}.tx" ] && [ -e "$P/${c##*/}.rx" ] && break; sleep 0.1; done

    # RXLIMIT: maximum size (KiB) of files written by Rx, to provoke a write error ("disk full")
    ( [ -n "$RXLIMIT" ] && { ulimit -f "$RXLIMIT"; trap '' XFSZ; }
      exec "$DRV" "$P/${c##*/}.rx" 9600 rx "$out" "$tries" "$to" ) > "$c/rx.log" 2>&1 & local rx=$!
    sleep 0.3
    "$DRV" "$P/${c##*/}.tx" 9600 tx "$file" "$tries" "$to" > "$c/tx.log" 2>&1 & local tx=$!

    local t=0
    while { kill -0 $tx 2>/dev/null || kill -0 $rx 2>/dev/null; } && [ $t -lt $((maxwait * 10)) ]; do sleep 0.1; t=$((t+1)); done
    if kill -0 $tx 2>/dev/null || kill -0 $rx 2>/dev/null; then ok=0; why="$why HANG(>${maxwait}s);"; kill -9 $tx $rx 2>/dev/null; fi
    kill $relay 2>/dev/null; wait $relay 2>/dev/null
    local secs=$((t / 10))

    if [ "$expect" = ok ]; then
        if [ ! -f "$out" ]; then ok=0; why="$why no output file;"
        elif [ "$(sha "$file")" != "$(sha "$out")" ]; then ok=0; why="$why SHA-256 differs;"; fi
        grep -q "FAILED" "$c/tx.log" "$c/rx.log" && { ok=0; why="$why FAILED reported;"; }
    else
        [ -e "$out" ] && [ -z "$KEEP_OUT_OK" ] && { ok=0; why="$why output file exists after failure;"; }
        [ -e "$out.part" ] && { ok=0; why="$why .part left behind;"; }
        grep -q "FAILED" "$c/tx.log" "$c/rx.log" || { ok=0; why="$why no FAILED reported;"; }
    fi
    grep -qE "AddressSanitizer|runtime error|Segmentation" "$c"/*.log && { ok=0; why="$why SANITIZER/CRASH;"; }
    [ -n "$TXGREP" ] && ! grep -qE "$TXGREP" "$c/tx.log" && { ok=0; why="$why tx.log lacks /$TXGREP/;"; }
    [ -n "$RXGREP" ] && ! grep -qE "$RXGREP" "$c/rx.log" && { ok=0; why="$why rx.log lacks /$RXGREP/;"; }
    [ -n "$RELAYGREP" ] && ! grep -qE "$RELAYGREP" "$c/relay.log" && { ok=0; why="$why relay.log lacks /$RELAYGREP/;"; }

    if [ $ok = 1 ]; then PASS=$((PASS+1)); printf 'PASS  %-34s (%ss)\n' "$name" "$secs"
    else FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES $name"; printf 'FAIL  %-34s (%ss) %s  [logs: %s]\n' "$name" "$secs" "$why" "$c"; KEEPW=1; fi
    unset TRIES TIMEOUT EXPECT MAXWAIT TXGREP RXGREP RELAYGREP RXOUT KEEP_OUT_OK RXLIMIT
}

# Records the outcome of a check made outside run_case.
verdict() { # verdict NAME STATUS(0=ok)
    if [ "$2" = 0 ]; then PASS=$((PASS+1)); echo "PASS  $1"
    else FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES $1"; echo "FAIL  $1"; KEEPW=1; fi
}

echo "== data transparency / file types (no faults) =="
run_case text         "$D/text.txt"
run_case binary       "$D/binary.bin"
run_case empty        "$D/empty.bin"
run_case small        "$D/small.bin"
run_case penguin      "$D/penguin.gif"
run_case zeros_0x00   "$D/zeros.bin"
run_case flags_0x7E   "$D/flags.bin"
run_case escapes_0x7D "$D/esc.bin"
run_case mixed_7E_7D  "$D/mixed.bin"
run_case frame_limit  "$D/nearmax.bin"
run_case random_200k  "$D/random200k.bin"
run_case large_1MiB   "$D/large.bin"

echo "== connection establishment =="
TXGREP="no UA" RELAYGREP="SET#1: drop" run_case set_lost "$D/f30k.bin" tx2rx:SET:1:drop
TXGREP="no UA" RELAYGREP="UA#1: drop" run_case ua_lost "$D/f30k.bin" rx2tx:UA:1:drop
RELAYGREP="SET#1: dup" run_case set_duplicated "$D/f30k.bin" tx2rx:SET:1:dup
RELAYGREP="garbage" run_case garbage_before_frames "$D/f30k.bin" tx2rx:SET:1:garbage tx2rx:I:2:garbage rx2tx:RR:1:garbage

echo "== information transfer: loss, corruption, duplicates =="
TXGREP="retransmissions +: [1-9]" run_case i_frame_lost "$D/f30k.bin" tx2rx:I:3:drop
TXGREP="retransmissions +: [1-9]" RXGREP="duplicates .*: [1-9]" run_case rr_lost "$D/f30k.bin" rx2tx:RR:3:drop
TXGREP="REJ received +: [1-9]" RXGREP="REJ sent +: [1-9]" run_case bcc2_corrupt "$D/f30k.bin" tx2rx:I:4:data
RXGREP="Frames dropped \(BCC1\) +: [1-9]" TXGREP="Timeouts +: [1-9]" run_case bcc1_corrupt "$D/f30k.bin" tx2rx:I:4:bcc1
TXGREP="Timeouts +: [1-9]" RXGREP="REJ sent +: [1-9]" run_case rej_lost "$D/f30k.bin" tx2rx:I:4:data rx2tx:REJ:1:drop
TXGREP="Timeouts +: [1-9]" run_case truncated_frame "$D/f30k.bin" tx2rx:I:5:trunc
TXGREP="REJ received +: [1-9]" run_case incomplete_escape "$D/f30k.bin" tx2rx:I:5:escend
TXGREP="REJ received +: [1-9]" run_case invalid_escape "$D/f30k.bin" tx2rx:I:5:badesc
RXGREP="duplicates .*: [1-9]" run_case i_frame_duplicated "$D/f30k.bin" tx2rx:I:3:dup tx2rx:I:4:dup
run_case rr_duplicated "$D/f30k.bin" rx2tx:RR:3:dup
TXGREP="retransmissions +: [1-9]" RXGREP="duplicates .*: [1-9]" run_case rr_delayed_late "$D/f30k.bin" rx2tx:RR:3:delay:1500
TXGREP="retransmissions +: [1-9]" run_case burst_of_errors "$D/f30k.bin" tx2rx:I:3-5:data rx2tx:RR:8:drop tx2rx:I:9:bcc1
run_case flags_with_errors "$D/flags.bin" tx2rx:I:2:data tx2rx:I:5:drop rx2tx:RR:7:drop
run_case start_end_lost "$D/small.bin" tx2rx:I:1:drop rx2tx:RR:3:drop

echo "== termination =="
TXGREP="no DISC" run_case disc_lost "$D/small.bin" tx2rx:DISC:1:drop
TXGREP="no DISC" RXGREP="no UA|final UA" run_case disc_reply_lost "$D/small.bin" rx2tx:DISC:1:drop
RXGREP="final UA not received" run_case final_ua_lost "$D/small.bin" tx2rx:UA:1-:drop
RXGREP="duplicates .*: [1-9]" run_case end_rr_lost "$D/small.bin" rx2tx:RR:3:drop

echo "== permanent failures / bounded termination =="
EXPECT=fail MAXWAIT=40 run_case cable_cut_during_transfer "$D/f30k.bin" tx2rx:I:5-:drop
EXPECT=fail MAXWAIT=40 run_case cable_cut_both_ways "$D/f30k.bin" tx2rx:I:5-:drop rx2tx:RR:4-:drop
EXPECT=fail MAXWAIT=40 run_case no_ack_at_all "$D/f30k.bin" rx2tx:RR:1-:drop
EXPECT=fail MAXWAIT=40 run_case always_corrupt "$D/f30k.bin" tx2rx:I:3-:data
EXPECT=fail MAXWAIT=40 run_case set_never_answered "$D/f30k.bin" rx2tx:UA:1-:drop

echo "== a failed transfer must not replace an existing output file =="
mkdir -p "$W/preserve"
echo "ORIGINAL CONTENT" > "$W/preserve/keep.bin"
EXPECT=fail MAXWAIT=40 RXOUT="$W/preserve/keep.bin" KEEP_OUT_OK=1 run_case preserve_existing_file "$D/f30k.bin" tx2rx:I:5-:drop
[ "$(cat "$W/preserve/keep.bin")" = "ORIGINAL CONTENT" ]; verdict preserve_existing_file_content $?

echo "== system errors =="
# input file missing: Tx reports it and exits
c=$W/nofile; mkdir -p "$c"
"$B/relay" "$P/${c##*/}.tx" "$P/${c##*/}.rx" 2>/dev/null & relay=$!
sleep 0.5
"$DRV" "$P/${c##*/}.tx" 9600 tx "$W/does-not-exist" 3 1 > "$c/tx.log" 2>&1
kill $relay 2>/dev/null
grep -q "No such file" "$c/tx.log" && grep -q "FAILED" "$c/tx.log"; verdict input_file_missing $?

# serial port missing
"$DRV" "$P/nonexistent-port" 9600 tx "$D/small.bin" 3 1 > "$W/p1.log" 2>&1
"$DRV" "$P/nonexistent-port" 9600 rx "$W/p.out" 3 1 > "$W/p2.log" 2>&1
grep -q "No such file" "$W/p1.log" && grep -q "No such file" "$W/p2.log" && ! grep -qE "Sanitizer|runtime error" "$W"/p?.log; verdict serial_port_missing $?

# unreadable input file
if [ "$(id -u)" != 0 ]; then
    cp "$D/small.bin" "$W/noperm.bin"; chmod 000 "$W/noperm.bin"
    "$DRV" "$P/nonexistent-port" 9600 tx "$W/noperm.bin" 3 1 > "$W/p3.log" 2>&1
    grep -q "Permission denied" "$W/p3.log"; verdict input_file_unreadable $?
    chmod 644 "$W/noperm.bin"
fi

# output cannot be created: both sides must end, nothing written
EXPECT=fail MAXWAIT=40 RXOUT="$W/no-such-dir/out.bin" run_case output_not_writable "$D/f30k.bin"

# write error while receiving (file size limit reached, same effect as a full disk)
EXPECT=fail MAXWAIT=40 RXLIMIT=8 RXGREP="fwrite|File too large" run_case disk_full_write_error "$D/f30k.bin"

echo "== premature termination of one process =="
for who in tx rx; do
    c=$W/kill_$who; mkdir -p "$c"
    # every RR delayed 20 ms: the 200 KB transfer takes ~4 s, long enough to kill a process in the middle
    "$B/relay" "$P/${c##*/}.tx" "$P/${c##*/}.rx" rx2tx:RR:*:delay:20 2>/dev/null & relay=$!
    for _ in $(seq 50); do [ -e "$P/${c##*/}.tx" ] && [ -e "$P/${c##*/}.rx" ] && break; sleep 0.1; done
    "$DRV" "$P/${c##*/}.rx" 9600 rx "$c/out.bin" 3 1 > "$c/rx.log" 2>&1 & rx=$!
    sleep 0.3
    "$DRV" "$P/${c##*/}.tx" 9600 tx "$D/random200k.bin" 3 1 > "$c/tx.log" 2>&1 & tx=$!
    sleep 1.5
    if [ $who = tx ]; then kill -9 $tx; survivor=$rx; log=rx.log; else kill -9 $rx; survivor=$tx; log=tx.log; fi
    t=0
    while kill -0 $survivor 2>/dev/null && [ $t -lt 400 ]; do sleep 0.1; t=$((t+1)); done
    hung=0
    if kill -0 $survivor 2>/dev/null; then hung=1; kill -9 $survivor; fi
    kill $relay 2>/dev/null
    # The final file must never exist. The .part file is removed by a surviving Rx;
    # a SIGKILLed Rx obviously cannot clean up after itself.
    partOk=1; [ $who = tx ] && [ -e "$c/out.bin.part" ] && partOk=0
    [ $hung = 0 ] && grep -q FAILED "$c/$log" && [ ! -e "$c/out.bin" ] && [ $partOk = 1 ] && ! grep -qE "Sanitizer|runtime error" "$c/$log"
    verdict "${who}_killed_mid_transfer(survivor_ended_in_$((t/10)).$((t%10))s)" $?
done

echo "== milestone 1: string exchange with the serial port API only =="
c=$W/m1; mkdir -p "$c"
"$B/relay" "$P/${c##*/}.tx" "$P/${c##*/}.rx" 2>/dev/null & relay=$!
for _ in $(seq 50); do [ -e "$P/${c##*/}.tx" ] && [ -e "$P/${c##*/}.rx" ] && break; sleep 0.1; done
"$B/m1_strings" rx "$P/${c##*/}.rx" > "$c/rx.log" 2>&1 & rx=$!
sleep 0.3
"$B/m1_strings" tx "$P/${c##*/}.tx"; txst=$?
wait $rx; rxst=$?
kill $relay 2>/dev/null
[ $txst = 0 ] && [ $rxst = 0 ]; verdict m1_strings $?

echo
echo "integration: $PASS passed, $FAIL failed${FAILED_NAMES:+ ($FAILED_NAMES )}"
[ -n "$KEEPW" ] && echo "logs kept in $W"
[ $FAIL = 0 ]
