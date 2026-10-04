#!/bin/sh
# Compare the screens of two builds at several frames after a reset (PPSSPP).
#
#   tools/regress.sh <base EBOOT> <new EBOOT> [frame...]   (default 1500 2600 3300 4000)
#   The base build is always captured with "ge off" (the CPU path is the
#   reference); CAPF_PRE (default "ge on") applies to the new build only.
#   DISK=/PSP/... boots that floppy.
#
# Base captures are cached in roms/cap/cpu-<md5 of base EBOOT>[-<disk>]/,
# the new ones land in roms/cap/new[-<disk>]/.
set -e
cd "$(dirname "$0")/.."
BASE=$1 NEW=$2
shift 2
FRAMES=${*:-1500 2600 3300 4000}
bdir=roms/cap/cpu-$(md5 -q "$BASE")${DISK:+-$(basename "$DISK")}
ndir=roms/cap/new${DISK:+-$(basename "$DISK")}
mkdir -p "$bdir" "$ndir"
fail=0
for f in $FRAMES; do
	[ -f "$bdir/$f.raw" ] || CAPF_PRE="ge off" EBOOT="$BASE" tools/capf.sh "$f" "$bdir/$f.raw" >/dev/null
	CAPF_PRE="${CAPF_PRE:-ge on}" EBOOT="$NEW" tools/capf.sh "$f" "$ndir/$f.raw" >/dev/null
	if cmp -s "$bdir/$f.raw" "$ndir/$f.raw"; then
		echo "frame $f: same"
	else
		echo "frame $f: DIFFERENT"
		fail=1
	fi
done
exit $fail
