#!/bin/sh
# Host check that fmgen (OPM + timers) output is unchanged between two
# revisions: builds fmdrv.cpp against each revision's fmgen and compares the
# hash of random register writes / timer counts / mixes at all rates.
#   tools/exacttest/fmcompare.sh <old-rev> [<new-rev>|WORKTREE] [iterations]
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
old=$1; new=${2:-WORKTREE}; n=${3:-2000000}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
build() {	# build <rev> <out>
	if [ "$1" = WORKTREE ]; then src=$top; else
		mkdir -p "$tmp/$1"; (cd "$top" && git archive "$1" fmgen x11/common.h win32api) | tar -x -C "$tmp/$1"; src=$tmp/$1
	fi
	c++ -O2 -w -I "$src/x11" -I "$src/win32api" -I "$src/fmgen" -o "$2" \
		"$here/fmdrv.cpp" "$src/fmgen/opm.cpp" "$src/fmgen/fmgen.cpp" "$src/fmgen/fmtimer.cpp"
}
build "$old" "$tmp/a"; build "$new" "$tmp/b"
a=$("$tmp/a" "$n"); b=$("$tmp/b" "$n")
echo "$old: $a"; echo "$new: $b"
[ "$a" = "$b" ] && echo SAME || { echo DIFFERENT; exit 1; }
