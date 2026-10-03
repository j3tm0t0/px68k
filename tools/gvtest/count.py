#!/usr/bin/env python3
# Count the MIPS instructions the PSP build spends per line on a Gradius-like
# 16 colour, 4 page setup (page 3 opaque, then 2, 1, 0; own X scroll each).
# usage: ELF=t.elf count.py [density-of-pages-0..2 (0..1)]
import sys, os, random
os.chdir(os.path.dirname(os.path.abspath(__file__)))
dens = float(sys.argv[1]) if len(sys.argv) > 1 else 0.1
sys.argv = ['emu.py', '0']
exec(open('emu.py').read().replace('sys.exit(', 'print('))

rng = random.Random(3)
data = bytearray(0x80000)
for i in range(0, 0x80000, 2):
    w = rng.randrange(1, 16) << 12                       # page 3: dense
    for p in range(3):
        if rng.random() < dens:
            w |= rng.randrange(1, 16) << (4 * p)
    data[i] = w & 0xff; data[i + 1] = w >> 8
uc.mem_write(GV, bytes(data))
set_scalar('TextDotX', ctypes.c_uint, 256, 4)
w8('CRTC_Regs', 0x29, 0x10)
icount = [0]
uc.hook_add(UC_HOOK_BLOCK, lambda u, a, sz, d: icount.__setitem__(0, icount[0] + sz // 4))

def run(multi):
    icount[0] = 0
    lines = 64
    for v in range(lines):
        set_scalar('VLINE', ctypes.c_uint, v * 3, 4)
        for i, x in enumerate((0x27e, 0xfc, 0x13f, 0xef)):
            w32('GrphScrollX', 4 * i, x + v); w32('GrphScrollY', 4 * i, 0)
        if multi:
            call('Grp_DrawLine4Multi', 3 | 2 << 2 | 1 << 4 | 0 << 6, 4)
        else:
            call('Grp_DrawLine4', 3, 1)
            for p in (2, 1, 0):
                call('Grp_DrawLine4', p, 0)
    return icount[0] / lines

print('density %.2f: 4 x Grp_DrawLine4 %.0f insns/line' % (dens, run(False)))
if 'Grp_DrawLine4Multi' in sym:
    print('density %.2f: Grp_DrawLine4Multi %.0f insns/line' % (dens, run(True)))
if os.environ.get('HIST'):
    hist = {}
    uc.hook_add(UC_HOOK_BLOCK, lambda u, a, sz, d: hist.__setitem__((a, sz), hist.get((a, sz), 0) + 1))
    run(True)
    tot = sum(sz // 4 * c for (a, sz), c in hist.items())
    for (a, sz), c in sorted(hist.items(), key=lambda kv: -kv[0][1] // 4 * kv[1])[:int(os.environ.get("HIST"))]:
        print('%08x %3d insns x %6d = %5.1f%%' % (a, sz // 4, c, 100.0 * sz // 4 * c / tot))
