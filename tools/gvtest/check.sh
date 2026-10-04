#!/bin/sh
# Check the graphic line decoders of x68k/gvram.c against the original ones
# (x68k/gvram.c of 513a603):
#  - host: main.c runs the original and the current C code side by side
#  - PSP: x68k/gvram.o as built by Makefile.psp runs in unicorn (emu.py) and
#    is compared with the original built for the host; count.py counts the
#    MIPS instructions per line on a Gradius-like 4 page setup.
# Needs python3 with unicorn and pyelftools (PYTHON=... to pick one).
# Run from the repository root.  usage: tools/gvtest/check.sh [cases]
set -e
T=tools/gvtest
PY=${PYTHON:-python3}
export PATH=~/pspdev/bin:$PATH

git show 513a603:x68k/gvram.c > $T/orig_gvram.c
python3 $T/prep.py $T/orig_gvram.c $T/old.c old_
python3 $T/prep.py x68k/gvram.c $T/new.c new_
cc -O2 -w -o $T/harness $T/main.c $T/old.c $T/new.c
$T/harness ${1:-1000000} | tail -1
cc -O2 -w -shared -fPIC -o $T/libref.dylib $T/ref.c $T/old.c

make -f Makefile.psp x68k/gvram.o >/dev/null
psp-gcc -G0 -O2 -fno-tree-loop-distribute-patterns -nostdlib -static \
	-Wl,-Ttext=0x08900000 -Wl,-e,_start -o $T/t.elf $T/pstub.c x68k/gvram.o
$PY $T/emu.py 20000 | tail -1
for d in 0.02 0.05 0.3 0.9; do
	$PY $T/count.py $d | tail -2
done
