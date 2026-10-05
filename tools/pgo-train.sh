#!/bin/sh
# Train the profile-guided build (Makefile.psp, PGO): build with PGO=gen, run
# it in PPSSPP (tools/ppsspp-run.sh), play Gradius (title, 1 player game,
# fire and move), run the attract demos of the disks in $DISKS, dump the
# counts with the debug command "gcov" and copy them to pgo/.  The counts do
# not depend on the speed, so PPSSPP trains as well as the device.
#
#   tools/pgo-train.sh		then: make -f Makefile.psp clean; make -f Makefile.psp
#
# Expects the PPSSPP memory stick set up as for ppsspp-run.sh, with
# disk/gradius.hdm and the disks of DISKS (default test_ch68.xdf
# test_sion4.xdf) in PSP/GAME/PX68K/disk.  PPSSPP's px68k config is put back
# afterwards.
set -e
cd "$(dirname "$0")/.."
MS="$HOME/.config/ppsspp/PSP/GAME/PX68K"
DISKS=${DISKS-"test_ch68.xdf test_sion4.xdf"}
export PSP_HOST=127.0.0.1

send() {
	/usr/bin/python3 - "$@" <<'EOF'
import importlib.util, sys, time
spec = importlib.util.spec_from_file_location("pd", "tools/psp-debug.py")
pd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pd)
s = pd.connect(retries=10)
s.sendall((" ".join(sys.argv[1:]) + "\n").encode())
time.sleep(1.5)  # closing at once can drop the command
s.close()
EOF
}

make -f Makefile.psp clean >/dev/null
make -f Makefile.psp PGO=gen >/dev/null
cp "$MS/.keropi/config" /tmp/pgo-train-config.$$
rm -rf "$MS/pgo"
mkdir -p "$MS/pgo"	# libgcov makes no directories
tools/ppsspp-run.sh

send fdd 0 /PSP/GAME/PX68K/disk/gradius.hdm
send reset
sleep 30			# boot, title
send pad circle 300; sleep 5	# 1 player game
send pad circle 300; sleep 10
for i in 1 2 3 4 5 6; do	# play: fire, move
	send pad circle+up 3000; sleep 4
	send pad triangle+down 3000; sleep 4
done
for d in $DISKS; do		# attract demos
	send fdd 0 /PSP/GAME/PX68K/disk/$d
	send reset
	sleep 60
done
send gcov
sleep 3
pkill -x PPSSPPSDL || true
cp /tmp/pgo-train-config.$$ "$MS/.keropi/config"
rm -f /tmp/pgo-train-config.$$

n=$(ls "$MS/pgo" | grep -c '\.gcda$' || true)
[ "$n" -gt 0 ] || { echo "pgo-train: no .gcda written"; exit 1; }
rm -rf pgo
mkdir pgo
cp "$MS"/pgo/*.gcda pgo/
make -f Makefile.psp clean >/dev/null
echo "pgo-train: $n profiles in pgo/"
