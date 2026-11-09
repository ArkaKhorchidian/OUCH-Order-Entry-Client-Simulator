#!/usr/bin/env bash
# Two clients against the simulator, including a dropped connection and
# gap-free recovery. Run from the repo root after building:
#   cmake -B build -G Ninja && cmake --build build && ./examples/demo.sh
set -euo pipefail
BIN=${BIN:-./build}
PORT=${PORT:-15000}
OUT=$(mktemp -d)

"$BIN/ouch_sim" --port "$PORT" --user ALICE:pw1 --user BOB:pw2 --symbols AAPL,MSFT > "$OUT/sim.log" 2>&1 &
SIM=$!
trap 'kill -INT $SIM 2>/dev/null || true' EXIT
sleep 0.3

# Alice rests an offer, loses her connection, reconnects, then cancels the rest.
( echo "sell AAPL 500 187.50"; sleep 0.3; echo "drop"; sleep 1.2; echo "reconnect"; sleep 0.5
  echo "orders"; sleep 0.2; echo "cancel 1"; sleep 0.3; echo "quit" ) |
  "$BIN/ouch_client" --port "$PORT" --user ALICE --password pw1 > "$OUT/alice.log" 2>&1 &
ALICE=$!
sleep 0.6

# Bob trades against Alice while she is offline, then sends a bad symbol.
( echo "buy AAPL 200 187.55"; sleep 0.2; echo "buy AAPL 100 187.50 ioc"; sleep 0.2
  echo "buy TSLA 10 250.00"; sleep 0.3; echo "quit" ) |
  "$BIN/ouch_client" --port "$PORT" --user BOB --password pw2 > "$OUT/bob.log" 2>&1

wait $ALICE
echo "=== ALICE"; cat "$OUT/alice.log"
echo; echo "=== BOB"; cat "$OUT/bob.log"
