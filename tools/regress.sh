#!/bin/sh
# Compare the screens of two builds at several frames after a reset (PPSSPP).
#
#   tools/regress.sh <base EBOOT> <new EBOOT> [frame...]   (default 1500 2600 3300 4000)
#   CAPF_PRE="ge on" applies to the new build only.
#
# Base captures are cached in roms/cap/<md5 of base EBOOT>/.
set -e
cd "$(dirname "$0")/.."
BASE=$1 NEW=$2
shift 2
FRAMES=${*:-1500 2600 3300 4000}
bdir=roms/cap/$(md5 -q "$BASE")
mkdir -p "$bdir" roms/cap/new
fail=0
for f in $FRAMES; do
	[ -f "$bdir/$f.raw" ] || CAPF_PRE= EBOOT="$BASE" tools/capf.sh "$f" "$bdir/$f.raw" >/dev/null
	EBOOT="$NEW" tools/capf.sh "$f" "roms/cap/new/$f.raw" >/dev/null
	if cmp -s "$bdir/$f.raw" "roms/cap/new/$f.raw"; then
		echo "frame $f: same"
	else
		echo "frame $f: DIFFERENT"
		fail=1
	fi
done
exit $fail
