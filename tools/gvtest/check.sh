#!/bin/sh
# Check the graphic line decoders of x68k/gvram.c against the original ones
# (x68k/gvram.c of master): main.c runs the original and the current C code
# side by side on random GVRAM, palettes, scrolls and register settings.
# Run from the repository root.  usage: tools/gvtest/check.sh [cases]
set -e
T=tools/gvtest

git show master:x68k/gvram.c > $T/orig_gvram.c
python3 $T/prep.py $T/orig_gvram.c $T/old.c old_
python3 $T/prep.py x68k/gvram.c $T/new.c new_
cc -O2 -w -o $T/harness $T/main.c $T/old.c $T/new.c
$T/harness ${1:-1000000} | tail -1
