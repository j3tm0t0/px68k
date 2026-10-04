/*
 * c68kbench.c: speed of the C68K core alone (debug command "cpubench").
 *
 * Runs small 68000 loops in a corner of main RAM for a number of cycles,
 * in C68k_Exec calls of `slice' cycles like the emulator, and returns the
 * host time per million 68000 cycles (10 MHz real time: 100000 us).  The
 * CPU state, the RAM used and the bus error flags are put back afterwards;
 * the loops touch nothing else (program 3 reads MFP GPIP, which has no
 * side effect).
 */
#include <string.h>
#include "c68k.h"

extern UINT8 *MEM;
extern UINT32 BusErrFlag, BusErrAdr, BusErrHandling, MemByteAccess;

#define BENCH_BASE	0x00080000	/* code; data at +0x1000 */
#define BENCH_SIZE	0x2000

/* 68000 words; "A" / "B" are patched to the absolute addresses of the data */
static const UINT16 prog_reg[] = {	/* registers only */
	0x7e00,			/* L: moveq #0,d7 */
	0x7203,			/*    moveq #3,d1 */
	0xd481,			/*    add.l d1,d2 */
	0xe58b,			/*    lsl.l #2,d3 */
	0xb543,			/*    eor.w d2,d3 */
	0x5244,			/*    addq.w #1,d4 */
	0xba44,			/*    cmp.w d4,d5 */
	0x6702,			/*    beq.s +2 */
	0x3c03,			/*    move.w d3,d6 */
	0x51c8, 0xffec,		/*    dbra d0,L */
	0x60e8,			/*    bra.s L */
};
static const UINT16 prog_ram[] = {	/* typical RAM traffic */
	0x41f9, 0, 0,		/* S: lea A,a0 */
	0x43f9, 0, 0,		/*    lea B,a1 */
	0x703f,			/*    moveq #63,d0 */
	0x2218,			/* L: move.l (a0)+,d1 */
	0xd481,			/*    add.l d1,d2 */
	0x32c2,			/*    move.w d2,(a1)+ */
	0x1628, 0x0004,		/*    move.b 4(a0),d3 */
	0xc67c, 0x00ff,		/*    and.w #$ff,d3 */
	0xe24b,			/*    lsr.w #1,d3 */
	0x3d43, 0xfffe,		/*    move.w d3,-2(a6) */
	0x51c8, 0xffea,		/*    dbra d0,L */
	0x60d8,			/*    bra.s S */
};
static const UINT16 prog_io[] = {	/* polling MFP GPIP */
	0x0839, 0x0007, 0x00e8, 0x8001,	/* L: btst #7,$e88001 */
	0x60f6,				/*    bra.s L */
};

static void put(UINT32 adr, const UINT16 *p, int n)
{
	int i;
	for (i = 0; i < n; i++)
		*(UINT16 *)(MEM + adr + i * 2) = p[i];
}

/*
 * prog: 0 registers, 1 RAM, 2 GPIP poll.  Returns microseconds per
 * million 68000 cycles; *insns: instructions per million cycles (estimate
 * from the loop's cycle count is left to the caller).
 */
unsigned C68k_Bench(int prog, int mcycles, int slice, unsigned (*now_us)(void))
{
	static UINT8 save_mem[BENCH_SIZE];
	c68k_struc save_cpu;
	UINT32 bef = BusErrFlag, bea = BusErrAdr, beh = BusErrHandling, mba = MemByteAccess;
	extern int m68000_ICountBk;
	int icbk = m68000_ICountBk;
	unsigned t0, t1;
	long long left;
	int k;

	memcpy(save_mem, MEM + BENCH_BASE, BENCH_SIZE);
	memcpy(&save_cpu, &C68K, sizeof(C68K));

	memset(MEM + BENCH_BASE, 0, BENCH_SIZE);
	if (prog == 0) {
		put(BENCH_BASE, prog_reg, sizeof(prog_reg) / 2);
	} else if (prog == 1) {
		put(BENCH_BASE, prog_ram, sizeof(prog_ram) / 2);
		*(UINT16 *)(MEM + BENCH_BASE + 2) = (BENCH_BASE + 0x1000) >> 16;
		*(UINT16 *)(MEM + BENCH_BASE + 4) = (BENCH_BASE + 0x1000) & 0xffff;
		*(UINT16 *)(MEM + BENCH_BASE + 8) = (BENCH_BASE + 0x1400) >> 16;
		*(UINT16 *)(MEM + BENCH_BASE + 10) = (BENCH_BASE + 0x1400) & 0xffff;
	} else {
		put(BENCH_BASE, prog_io, sizeof(prog_io) / 2);
	}
	for (k = 0; k < 8; k++)
		C68K.D[k] = 0;
	C68K.D[0] = 1000;
	C68K.D[5] = 77;
	C68K.A[6] = BENCH_BASE + 0x1c00;
	C68K.IRQLine = 0;
	C68K.HaltState = 0;
	C68k_Set_Reg(&C68K, C68K_SR, 0x2700);
	C68k_Set_Reg(&C68K, C68K_PC, BENCH_BASE);
	BusErrFlag = BusErrHandling = 0;

	left = (long long)mcycles * 1000000;
	t0 = now_us();
	while (left > 0) {
		C68K.ICount = m68000_ICountBk = 0;
		left -= C68k_Exec(&C68K, slice);
	}
	t1 = now_us();

	memcpy(&C68K, &save_cpu, sizeof(C68K));
	memcpy(MEM + BENCH_BASE, save_mem, BENCH_SIZE);
	BusErrFlag = bef; BusErrAdr = bea; BusErrHandling = beh; MemByteAccess = mba;
	m68000_ICountBk = icbk;
	return (t1 - t0) / (unsigned)mcycles;
}
