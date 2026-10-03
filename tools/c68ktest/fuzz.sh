#!/bin/sh
# Differential test of the 68000 core and x68k/mem_wrap.c.
#
#   tools/c68ktest/fuzz.sh <base commit> [new commit]	(default new: the working tree)
#
# Builds harness.c against the m68000/ and x68k/mem_wrap.c of both versions
# (i386 Linux in docker: the core needs 32-bit pointers) and compares the
# hashes of random instruction runs, generated idle loops and an IPL ROM run
# (roms/IPLROM.DAT if present).  Any difference in registers, flags, cycles,
# device accesses (order included), BusErrFlag/MemByteAccess or RAM shows.
#
# FUZZ_CFLAGS (default -DPSP) is passed to both builds, FUZZ_CFLAGS_A /
# FUZZ_CFLAGS_B to one each, e.g. to compare a build macro on one tree:
#   FUZZ_CFLAGS_B=-DC68K_NO_IDLE tools/c68ktest/fuzz.sh HEAD HEAD
set -e
cd "$(dirname "$0")/../.."
TOP=$(pwd)
IMG=px68k-i386
docker image inspect $IMG >/dev/null 2>&1 || docker build --platform linux/386 -t $IMG - <<'EOF'
FROM i386/debian:bookworm
RUN apt-get update && apt-get install -y gcc g++ make libsdl1.2-dev libsdl-gfx1.2-dev && rm -rf /var/lib/apt/lists/*
EOF
W=$(mktemp -d "${TMPDIR:-/tmp}/c68ktest.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir "$W/a" "$W/b"
git archive "$1" m68000 x68k x11 win32api fmgen | tar xf - -C "$W/a"
if [ -n "$2" ]; then
	git archive "$2" m68000 x68k x11 win32api fmgen | tar xf - -C "$W/b"
else
	cp -R m68000 x68k x11 win32api fmgen "$W/b/"
fi
cp tools/c68ktest/harness.c "$W/"
[ -f roms/IPLROM.DAT ] && cp roms/IPLROM.DAT "$W/"
for t in a b; do
	if [ $t = a ]; then x=$FUZZ_CFLAGS_A; else x=$FUZZ_CFLAGS_B; fi
	docker run --rm --platform linux/386 -v "$W":/w $IMG sh -c "gcc -O2 -w ${FUZZ_CFLAGS--DPSP} $x \
		-I/w/$t/x11 -I/w/$t/x68k -I/w/$t/fmgen -I/w/$t/win32api -I/w/$t/m68000 -o /w/h_$t \
		/w/harness.c /w/$t/m68000/c68k.c /w/$t/m68000/m68000.c /w/$t/x68k/mem_wrap.c"
done
fail=0
set -- "fuzz 2000000 1" "fuzz 2000000 7" "idle 1000000 3" "idle 1000000 4"
[ -f "$W/IPLROM.DAT" ] && set -- "$@" "ipl IPLROM.DAT 200000"
for args in "$@"; do
	a=$(docker run --rm --platform linux/386 -v "$W":/w $IMG sh -c "cd /w && ./h_a $args 2>&1 >/dev/null | tail -1")
	b=$(docker run --rm --platform linux/386 -v "$W":/w $IMG sh -c "cd /w && ./h_b $args 2>&1 >/dev/null | tail -1")
	if [ "$a" = "$b" ]; then echo "same $args: $a"; else echo "DIFFERENT $args: $a / $b"; fail=1; fi
done
exit $fail
