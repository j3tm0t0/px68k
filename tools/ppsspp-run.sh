#!/bin/sh
# Copy the current build into PPSSPP's memory stick and start it there.
#
#   tools/ppsspp-run.sh            (re)start PPSSPP with ./EBOOT.PBP
#
# Expects PSP/GAME/PX68K in PPSSPP's memory stick to hold debug.key, .keropi/
# (iplrom.dat, cgrom.dat, config) and disk/. Waits until the debug port
# listens; talk to it with PSP_HOST=127.0.0.1 tools/psp-debug.py.
set -e
cd "$(dirname "$0")/.."
PPSSPP=/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL
DIR="$HOME/.config/ppsspp/PSP/GAME/PX68K"

pkill -x PPSSPPSDL 2>/dev/null && sleep 1 || true
cp EBOOT.PBP "$DIR/EBOOT.PBP"
rm -f "$DIR/px68k.log"
"$PPSSPP" --windowed --escape-exit "$DIR/EBOOT.PBP" >/dev/null 2>&1 &
i=0
until grep -q 'listening' "$DIR/px68k.log" 2>/dev/null; do
	i=$((i + 1))
	[ $i -lt 60 ] || { echo "ppsspp-run: no debug port after 60 s"; exit 1; }
	sleep 1
done
echo "ppsspp-run: up"
