#!/bin/sh
# Check GVRAM_WriteWord against GVRAM_Write(adr, hi) + GVRAM_Write(adr + 1, lo)
# with the real x68k/gvram.c (i386 docker, as fuzz.sh).
set -e
cd "$(dirname "$0")/../.."
IMG=px68k-i386
docker image inspect $IMG >/dev/null 2>&1 || { echo "run fuzz.sh once to build $IMG"; exit 1; }
docker run --rm --platform linux/386 -v "$(pwd)":/w $IMG sh -c "cd /w &&
	gcc -O2 -w -Ix11 -Ix68k -Ifmgen -Iwin32api -Im68000 -c -o /tmp/gv.o x68k/gvram.c &&
	gcc -O2 -w -o /tmp/gvt tools/c68ktest/gvword.c /tmp/gv.o -static -Wl,--unresolved-symbols=ignore-all && /tmp/gvt"
