#!/bin/sh
# Start the current build in PPSSPP, capture the screen of frame N after one
# reset and print its md5. A fresh start each time: WinX68k_Reset() does not
# clear everything, so only the first reset of a run is reproducible.
#
#   tools/capf.sh <frame> [out.raw]      (EBOOT=... to use another build,
#                                         CAPF_PRE="ge on" to send a command first)
set -e
cd "$(dirname "$0")/.."
FRAME=$1
OUT=${2:-cap.raw}
CAP="$HOME/.config/ppsspp/PSP/GAME/PX68K/cap.raw"
rm -f "$CAP"
tools/ppsspp-run.sh >/dev/null
[ -z "$CAPF_PRE" ] || PSP_HOST=127.0.0.1 FOLLOW_SEC=1 /usr/bin/python3 tools/psp-debug.py cmd $CAPF_PRE >/dev/null
PSP_HOST=127.0.0.1 FOLLOW_SEC=1 /usr/bin/python3 tools/psp-debug.py cmd capf "$FRAME" >/dev/null
n=0
until [ -f "$CAP" ] && [ "$(stat -f %z "$CAP")" -gt 0 ]; do
	n=$((n + 1)); [ $n -lt 300 ] || { echo "capf: timed out"; exit 1; }
	sleep 2
done
sleep 1
cp "$CAP" "$OUT"
[ ! -f "${CAP%.raw}.state" ] || cp "${CAP%.raw}.state" "${OUT%.raw}.state"
md5 -q "$OUT"
