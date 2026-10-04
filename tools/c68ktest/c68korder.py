#!/usr/bin/env python3
# c68korder.py <hot list> <inline hot 0|1> <inline cold 0|1> [in [out]]: write m68000/c68k_op.c
# (or in to out) with the handlers of the list first (in list order: I-cache locality), and
# C68K_INL (RAM fast paths inlined or not, c68kmacro.h) set for the listed and for the other
# handlers.  Run from the repository root.  Makefile.psp builds the PSP's c68k_op_psp.c with it
# (hot995.txt 1 1); c68k_op.c keeps the original order (no gain elsewhere, see tools/bench).  hot995.txt / hot97.txt: handlers covering 99.5% / 97% of the instructions of
# host profiles of SION IV (weapon select), 超連射68K (attract demo) and Gradius (frames 3000-3300).
import re, sys
hot = [l.split()[0] for l in open(sys.argv[1]) if l.strip()]
inl_hot, inl_cold = sys.argv[2], sys.argv[3]
p = sys.argv[4] if len(sys.argv) > 4 else 'm68000/c68k_op.c'
q = sys.argv[5] if len(sys.argv) > 5 else p
s = open(p, 'rb').read().decode('latin-1')
# drop the "#if 0" alternative (keep the #else part)
i = s.index('#if 0\n'); j = s.index('#else\n', i); k = s.index('#endif\n', j)
s = s[:i] + s[j + len('#else\n'):k] + s[k + len('#endif\n'):]
t = s.index('#ifdef BUILD_NCDZPSP')
body, tail = s[:t], s[t:]
lines = body.split('\n')
first = next(n for n, l in enumerate(lines) if l.startswith('OP('))
head = '\n'.join(lines[:first]) + '\n'
chunks = []
cur = None
for l in lines[first:]:
    if l.startswith('OP('):
        if cur: chunks.append(cur)
        cur = [l]
    else:
        cur.append(l)
chunks.append(cur)
byname = {}
for c in chunks:
    name = re.match(r'OP\((\w+)\)', c[0]).group(1)
    byname[name] = c
assert all('#' not in l for c in chunks for l in c if l.startswith('#')), 'preprocessor line inside chunks'
hotset = [h for h in hot if h in byname]
out = [head, '/* hot handlers first (I-cache), see docs/c68k-speed.md */\n#undef C68K_INL\n#define C68K_INL %s\n\n' % inl_hot]
for h in hotset:
    out.append('\n'.join(byname[h]) + '\n')
out.append('\n#undef C68K_INL\n#define C68K_INL %s\n\n' % inl_cold)
for c in chunks:
    name = re.match(r'OP\((\w+)\)', c[0]).group(1)
    if name not in hotset:
        out.append('\n'.join(c) + '\n')
out.append('\n#undef C68K_INL\n#define C68K_INL 1\n\n')
out.append(tail)
open(q, 'wb').write(''.join(out).encode('latin-1'))
print('hot handlers:', len(hotset), 'of', len(chunks))
