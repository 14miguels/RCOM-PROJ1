#!/usr/bin/env bash
# RCOM 2026/2027 - protocol efficiency measurements (S = R / C)
#
# Uses the OFFICIAL virtual cable (bin/cable + socat) to vary
#   - frame size       (file bytes per DATA packet, -DAPP_CHUNK_SIZE)
#   - bit error rate   (cable command "ber")
#   - propagation delay (cable command "prop")
# and prints one CSV line per run. Nothing here is a made-up number: every
# value comes from the Tx statistics printed by llCloseTx.
#
#   ./tests/efficiency.sh [file]          (default: penguin.gif) from the project root
#   CHUNKS="100 500 997" BERS="0 0.00001" PROPS="0 50000" BAUD=9600 ./tests/efficiency.sh
#
# Output: tests/results/efficiency.csv (appended) and the same lines on stdout.
# Takes roughly 15-25 s per run at 9600 baud.

cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
FILE=${1:-$ROOT/penguin.gif}
CHUNKS=${CHUNKS:-"100 250 500 997"}
BERS=${BERS:-"0 0.00001"}
PROPS=${PROPS:-"0 50000"}   # microseconds
BAUD=${BAUD:-9600}
RETX=${RETX:-3}
TIMEOUT=${TIMEOUT:-4}
W=$(mktemp -d "${TMPDIR:-/tmp}/rcom-eff.XXXXXX") || exit 1
mkdir -p tests/results tests/build

[ -x bin/cable ] || make -B cable >/dev/null || exit 1
which socat >/dev/null || { echo "socat not found"; exit 1; }

cleanup() {
    pkill -P $$ 2>/dev/null
    pkill -f "socat -dd PTY,link=/tmp/ttyS1[01]" 2>/dev/null
    pkill -f "bin/cable" 2>/dev/null
    rm -rf "${W:?}"
}
trap cleanup EXIT

# Cable with commands fed through a FIFO.
mkfifo "$W/cmd"
( tail -f "$W/cmd" | ./bin/cable > "$W/cable.log" 2>&1 ) &
sleep 4
[ -e /tmp/ttyS10 ] && [ -e /tmp/ttyS11 ] || { echo "cable did not create /tmp/ttyS10 /tmp/ttyS11"; exit 1; }
cable() { echo "$*" > "$W/cmd"; sleep 1; }
cable "baud $BAUD"

FILE_BYTES=$(wc -c < "$FILE" | tr -d ' ')
OUT=tests/results/efficiency.csv
[ -s "$OUT" ] || echo "date,chunk_bytes,ber,prop_us,baud,file_bytes,elapsed_s,iframes_sent,retransmissions,timeouts,rej,S_link,S_file,ok" > "$OUT"

for chunk in $CHUNKS; do
    DRV=tests/build/driver_chunk$chunk
    gcc -Wall -Wextra -Wno-strict-prototypes -DAPP_CHUNK_SIZE=$chunk -o "$DRV" tests/driver.c src/application_layer.c src/link_layer.c src/serial_port.c || exit 1
    for ber in $BERS; do
        for prop in $PROPS; do
            cable "ber $ber"
            cable "prop $prop"
            rm -f "$W/rx.out" "$W/rx.log" "$W/tx.log"
            "$DRV" /tmp/ttyS11 "$BAUD" rx "$W/rx.out" "$RETX" "$TIMEOUT" > "$W/rx.log" 2>&1 &
            rx=$!
            sleep 1
            "$DRV" /tmp/ttyS10 "$BAUD" tx "$FILE" "$RETX" "$TIMEOUT" > "$W/tx.log" 2>&1
            wait $rx
            ok=no; cmp -s "$FILE" "$W/rx.out" && ok=yes
            getstat() { grep -E "$1" "$W/tx.log" | head -1 | sed -E 's/.*: *([0-9.]+).*/\1/'; }
            el=$(getstat "Elapsed time"); sent=$(getstat "I frames sent"); rt=$(getstat "retransmissions")
            to=$(getstat "^Timeouts"); rej=$(getstat "REJ received"); s=$(getstat "Efficiency S")
            sfile=$(awk -v b="$FILE_BYTES" -v t="${el:-0}" -v c="$BAUD" 'BEGIN{ if (t>0) printf "%.3f", b*8/t/c; else print "0" }')
            line="$(date +%F),$chunk,$ber,$prop,$BAUD,$FILE_BYTES,$el,$sent,$rt,$to,$rej,$s,$sfile,$ok"
            echo "$line" | tee -a "$OUT"
        done
    done
done
