#!/usr/bin/env python3
# Run the PSP-compiled Grp_DrawLine* (t.elf, linked from x68k/gvram.o) in
# unicorn and compare with the original C decoders built for the host
# (libref.dylib).  usage: emu.py [cases] [seed]
import ctypes, random, struct, sys, os
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.mips_const import *

here = os.path.dirname(os.path.abspath(__file__))
cases = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)

elf = ELFFile(open(os.path.join(here, os.environ.get('ELF', 't.elf')), 'rb'))
sym = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}

uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
uc.mem_map(0x08800000, 0x00400000)
STACK, MAGIC = 0x09000000, 0x08f00000
uc.mem_map(STACK - 0x10000, 0x20000)
uc.mem_map(MAGIC, 0x1000)
for seg in elf.iter_segments():
    if seg['p_type'] == 'PT_LOAD':
        uc.mem_write(seg['p_vaddr'], seg.data())

emulated = {'max': 0, 'min': 0}
def regval(n):
    return uc.reg_read(UC_MIPS_REG_0 + n) if n else 0
def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v
def on_invalid(uc_):
    # Allegrex max/min: SPECIAL, funct 0x2c / 0x2d
    pc = uc.reg_read(UC_MIPS_REG_PC)
    ins = struct.unpack('<I', uc.mem_read(pc, 4))[0]
    op, rs, rt, rd, fn = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31, ins & 63
    if op == 0 and fn in (0x2c, 0x2d):
        a, b = s32(regval(rs)), s32(regval(rt))
        r = max(a, b) if fn == 0x2c else min(a, b)
        emulated['max' if fn == 0x2c else 'min'] += 1
        if rd:
            uc.reg_write(UC_MIPS_REG_0 + rd, r & 0xffffffff)
        uc.reg_write(UC_MIPS_REG_PC, pc + 4)
        return True
    print('invalid instruction %08x at %08x' % (ins, pc))
    return False
uc.hook_add(UC_HOOK_INSN_INVALID, on_invalid)
blk = [0]
uc.hook_add(UC_HOOK_BLOCK, lambda u, a, sz, d: blk.__setitem__(0, a))
def on_intr(uc_, intno, data):
    # unicorn reports pc 0 here: find the Allegrex min/max in the current block
    a = blk[0]
    for _ in range(256):
        ins = struct.unpack('<I', uc.mem_read(a, 4))[0]
        if (ins >> 26) == 0 and (ins & 63) in (0x2c, 0x2d):
            break
        a += 4
    else:
        print('exception %d in block %08x' % (intno, blk[0]))
        uc.emu_stop()
        return
    uc.reg_write(UC_MIPS_REG_PC, a)
    if not on_invalid(uc_):
        uc.emu_stop()
uc.hook_add(UC_HOOK_INTR, on_intr)
def on_unmapped(uc_, access, addr, size, value, data):
    print('unmapped access %x at pc %x' % (addr, uc.reg_read(UC_MIPS_REG_PC)))
    return False
uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

def call(name, *args):
    for i, a in enumerate(args):
        uc.reg_write(UC_MIPS_REG_A0 + i, a & 0xffffffff)
    uc.reg_write(UC_MIPS_REG_SP, STACK)
    uc.reg_write(UC_MIPS_REG_RA, MAGIC)
    try:
        uc.emu_start(sym[name], MAGIC)
    except UcError:
        print('fault in', name, args)
        if os.environ.get('TRACE'):
            last = []
            uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: (last.append(a), last.__delitem__(0) if len(last) > 40 else None))
            try:
                for i, a in enumerate(args):
                    uc.reg_write(UC_MIPS_REG_A0 + i, a & 0xffffffff)
                uc.reg_write(UC_MIPS_REG_SP, STACK); uc.reg_write(UC_MIPS_REG_RA, MAGIC)
                uc.emu_start(sym[name], MAGIC)
            except UcError:
                pass
            print(' '.join('%x' % a for a in last))
        raise

ref = ctypes.CDLL(os.path.join(here, 'libref.dylib'))
def hv(name, ctype):
    return ctype.in_dll(ref, name)

PAD = 0x4000
GV = sym['GVRAM']

def w8(name, off, val): uc.mem_write(sym[name] + off, bytes([val & 0xff]))
def w16(name, off, val): uc.mem_write(sym[name] + off, struct.pack('<H', val & 0xffff))
def w32(name, off, val): uc.mem_write(sym[name] + off, struct.pack('<I', val & 0xffffffff))

host_gv = (ctypes.c_ubyte * (PAD + 0x80000 + PAD)).in_dll(ref, 'gv_store')
host_pal = (ctypes.c_ushort * (65536 + 1024)).in_dll(ref, 'pal_store')
host_lb = [(ctypes.c_ushort * 1024).in_dll(ref, n) for n in ('Grp_LineBuf', 'Grp_LineBufSP', 'Grp_LineBufSP2')]
host_regs = (ctypes.c_ubyte * 1024).in_dll(ref, 'Pal_Regs')
host_p16 = (ctypes.c_ushort * 65536).in_dll(ref, 'Pal16')
host_crtc = (ctypes.c_ubyte * 48).in_dll(ref, 'CRTC_Regs')
host_sx = (ctypes.c_uint * 4).in_dll(ref, 'GrphScrollX')
host_sy = (ctypes.c_uint * 4).in_dll(ref, 'GrphScrollY')

def rbytes(n):
    return bytes(rng.getrandbits(8) for _ in range(n)) if n < 4096 else rng.randbytes(n)

def fill_gvram(mode):
    data = bytearray(rng.randbytes(PAD + 0x80000 + PAD))
    if mode:
        keep = rng.randbytes(len(data))
        for i in range(0, len(data)):
            if keep[i] & 3:
                data[i] &= 0x0f if keep[i] & 4 else 0xf0
            if mode == 2 and keep[i] & 0x70:
                data[i] = 0
    uc.mem_write(GV, bytes(data[PAD:PAD + 0x80000]))
    ctypes.memmove(host_gv, bytes(data), len(data))

def fill_tables():
    pal = bytearray(rng.randbytes(2 * (256 + 65536 + 1024)))
    for i in range(16):
        if rng.random() < 0.3:
            pal[2 * i] = pal[2 * i + 1] = 0
    uc.mem_write(sym['GrphPal'], bytes(pal))
    ctypes.memmove(host_pal, bytes(pal), 2 * (65536 + 1024))
    regs = rng.randbytes(1024)
    uc.mem_write(sym['Pal_Regs'], regs); ctypes.memmove(host_regs, regs, 1024)
    p16 = rng.randbytes(131072)
    uc.mem_write(sym['Pal16'], p16); ctypes.memmove(host_p16, p16, 131072)

def set_scalar(name, ctype, val, width):
    hv(name, ctype).value = val
    (w16 if width == 2 else w32)(name, 0, val)

# Pal16Adr is set by GVRAM_Init on both sides
call('GVRAM_Init')
ref.old_GVRAM_Init()
fill_gvram(0); fill_tables()   # GVRAM_Init cleared GVRAM

names = ['Grp_DrawLine16', 'Grp_DrawLine8', 'Grp_DrawLine4', 'Grp_DrawLine4h', 'Grp_DrawLine16SP',
         'Grp_DrawLine8SP', 'Grp_DrawLine4SP', 'Grp_DrawLine4hSP', 'Grp_DrawLine8TR', 'Grp_DrawLine4TR',
         'Grp_DrawLine4Multi']
widths = [256, 384, 512, 768, 1024, 255, 257, 511, 513, 1, 2, 3]
fails = 0
per = [0] * len(names)
bad = [0] * len(names)
for c in range(cases):
    if c % 300 == 0:
        fill_gvram(c // 300 % 3)
        fill_tables()
    if rng.random() < 0.25:
        i = rng.randrange(16); v = 0 if rng.random() < 0.5 else rng.getrandbits(16)
        w16('GrphPal', 2 * i, v); host_pal[i] = v
    fn = rng.randrange(len(names) if 'Grp_DrawLine4Multi' in sym else len(names) - 1)
    page = rng.randrange(8)
    opaq = 0 if rng.random() < 0.34 else rng.randrange(3)
    for nm in ('Ibit', 'Pal_HalfMask', 'Pal_Ix2'):
        set_scalar(nm, ctypes.c_ushort, rng.getrandbits(16), 2)
    crtc = rng.randbytes(48)
    uc.mem_write(sym['CRTC_Regs'], crtc); ctypes.memmove(host_crtc, crtc, 48)
    dotx = rng.choice(widths) if rng.random() < 0.67 else 1 + rng.randrange(1024)
    set_scalar('TextDotX', ctypes.c_uint, dotx, 4)
    set_scalar('VLINE', ctypes.c_uint, rng.randrange(1024 if rng.random() < 0.5 else 600), 4)
    sx = [rng.getrandbits(32) if rng.random() < 0.25 else rng.randrange(1024) for _ in range(4)]
    sy = [rng.getrandbits(32) if rng.random() < 0.25 else rng.randrange(1024) for _ in range(4)]
    for i in range(4):
        if rng.random() < 0.125: sx[i] = 511 - rng.randrange(3)
        if rng.random() < 0.125: sx[i] &= ~1
    mpages, mn = rng.getrandbits(8), 1 + rng.randrange(4)
    if rng.random() < 0.5:
        # descending pages, as WinDraw_DrawLine with VCReg1 = e4, own scroll each
        mpages, mn = 0, 0
        for p in (3, 2, 1, 0):
            if rng.random() < 0.75:
                mpages |= p << (2 * mn); mn += 1
        if mn == 0:
            mpages, mn = 3, 1
    elif fn == 10 and rng.random() < 0.75:
        for i in range(1, 4):
            sx[i] = sx[0] + (0 if rng.random() < 0.875 else 0x200)
            sy[i] = sy[0] + (0 if rng.random() < 0.875 else 0x200)
    for i in range(4):
        sx[i] &= 0xffffffff; sy[i] &= 0xffffffff
        host_sx[i] = sx[i]; host_sy[i] = sy[i]
        w32('GrphScrollX', 4 * i, sx[i]); w32('GrphScrollY', 4 * i, sy[i])
    init = []
    for k in range(3):
        if k == 1:
            b = bytearray(rng.randbytes(2048))
            for i in range(1024):
                if rng.random() < 0.5: b[2 * i] = b[2 * i + 1] = 0
            b = bytes(b)
        else:
            b = rng.randbytes(2048)
        init.append(b)
        uc.mem_write(sym[['Grp_LineBuf', 'Grp_LineBufSP', 'Grp_LineBufSP2'][k]], b)
        ctypes.memmove(host_lb[k], b, 2048)
    # whatever surrounds GVRAM on the PSP side (read out of bounds by a few
    # decoders) is mirrored into the host's padding
    ctypes.memmove(host_gv, bytes(uc.mem_read(GV - PAD, PAD)), PAD)
    name = names[fn]
    def post():
        # the decoder tables live right after GVRAM in this link and are
        # (re)built at the start of a call: mirror them as the call left them
        ctypes.memmove(ctypes.addressof(host_gv) + PAD + 0x80000, bytes(uc.mem_read(GV + 0x80000, PAD)), PAD)
    if fn == 10:
        call(name, mpages, mn); post()
        for k in range(mn):
            ref.old_Grp_DrawLine4((mpages >> (k * 2)) & 3, 1 if k == 0 else 0)
    elif fn in (0, 3, 4, 7):
        call(name); post(); getattr(ref, 'old_' + name)()
    elif fn in (5, 6):
        call(name, page); post(); getattr(ref, 'old_' + name)(page)
    else:
        call(name, page, opaq); post(); getattr(ref, 'old_' + name)(page, opaq)
    per[fn] += 1
    for k, nm in enumerate(('Grp_LineBuf', 'Grp_LineBufSP', 'Grp_LineBufSP2')):
        if bytes(uc.mem_read(sym[nm], 2048)) != bytes(host_lb[k]):
            fails += 1
            bad[fn] += 1
            if fails <= 10:
                print('MISMATCH case %d %s page %d opaq %d dotx %d buf %s' % (c, name, page, opaq, dotx, nm))
                got = struct.unpack('<1024H', bytes(uc.mem_read(sym[nm], 2048)))
                want = struct.unpack('<1024H', bytes(host_lb[k]))
                diff = [i for i in range(1024) if got[i] != want[i]]
                vl = struct.unpack('<I', bytes(uc.mem_read(sym['VLINE'], 4)))[0]
                print('  pages %x n %d sx %s sy %s vline %d r29 %02x; dots %s' % (
                    mpages, mn, [hex(v) for v in sx], [hex(v) for v in sy], vl, crtc[0x29], diff[:20]))
            break
print(' '.join('%s:%d' % (n.replace('Grp_DrawLine', ''), p) for n, p in zip(names, per)))
print('bad', bad)
print('cases %d, mismatches %d, emulated allegrex max %d min %d' % (cases, fails, emulated['max'], emulated['min']))
sys.exit(1 if fails else 0)
