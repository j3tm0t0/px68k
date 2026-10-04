/*
 * core.c: speed of the C68K core with x68k/mem_wrap.c and x68k/gvram.c on
 * synthetic 68000 programs (no ROM, no disk image).
 *
 *   core [seconds per test]	prints "<test> <us per 1M 68000 cycles>"
 *
 * Every test runs its program in C68k_Exec calls of 200 cycles, as
 * WinX68k_Exec does (CLOCK_SLICE), after a warm-up.  The devices other than
 * GVRAM are stubs; MFP GPIP reads give a constant (as in
 * tools/c68ktest/harness.c, GPIP has no side effect and only changes
 * between slices).  Built by tools/bench/bench.py against the sources of
 * each variant (32-bit host: the core keeps host pointers in UINT32).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "c68k.h"

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;

extern BYTE *IPL, *MEM, *FONT;
BYTE SCSIIPL[0x10000];
BYTE SRAM[0x10000];
BYTE TVRAM[0x80000];
extern BYTE GVRAM[];			/* gvram.c */
extern DWORD BusErrFlag, MemByteAccess;
extern int m68000_ICountBk;
void m68000_init(void);

/* what gvram.c uses besides GVRAM */
BYTE CRTC_Regs[48];
BYTE TextDirtyLine[1024];
DWORD GrphScrollX[4], GrphScrollY[4];
DWORD VLINE, TextDotX = 768, TextDotY = 512;
WORD CRTC_FastClrMask;
WORD Pal16[65536];
WORD Ibit, Pal_HalfMask, Pal_Ix2;
BYTE Pal_Regs[1024];
WORD GrphPal[256];
BYTE VCReg0[2], VCReg1[2], VCReg2[2];

static unsigned devacc;		/* accesses to the stub devices */

#define DEV(name) \
	BYTE name##_Read(DWORD a) { devacc++; return 0xff; } \
	void name##_Write(DWORD a, BYTE v) { devacc++; }
DEV(ADPCM) DEV(BG) DEV(CRTC) DEV(DMA) DEV(FDC) DEV(IOC) DEV(Mcry) DEV(MIDI)
DEV(Pal) DEV(PIA) DEV(RTC) DEV(SASI) DEV(SCC) DEV(SCSI) DEV(SRAM) DEV(SysPort)
DEV(VCtrl)
BYTE MFP_Read(DWORD a) { if (a == 0xe88001) return 0x03; devacc++; return 0xff; }
void MFP_Write(DWORD a, BYTE v) { devacc++; }
BYTE TVRAM_Read(DWORD a) { devacc++; return TVRAM[(a & 0x7ffff) ^ 1]; }
void TVRAM_Write(DWORD a, BYTE v) { devacc++; TVRAM[(a & 0x7ffff) ^ 1] = v; }
BYTE OPM_Read(WORD a) { devacc++; return 0; }
void OPM_Write(DWORD r, BYTE v) { devacc++; }

/* 68000 memory: words in host order */
static void w16(DWORD a, WORD v) { *(WORD *)(MEM + a) = v; }
static void w32(DWORD a, DWORD v) { w16(a, v >> 16); w16(a + 2, v); }
static DWORD put(DWORD a, const WORD *p, int n) { while (n--) { w16(a, *p++); a += 2; } return a; }

#define TRAPADR	0x1f00		/* every vector: bra.s * */

static void reset_cpu(DWORD pc)
{
	int k;

	for (k = 0; k < 256; k++)
		w32(k * 4, TRAPADR);
	w16(TRAPADR, 0x60fe);
	C68k_Reset(&C68K);
	for (k = 0; k < 8; k++)
		C68K.D[k] = C68K.A[k] = 0;
	C68K.IRQLine = 0;
	C68K.HaltState = 0;
	C68k_Set_Reg(&C68K, C68K_SR, 0x2700);
	C68k_Set_Reg(&C68K, C68K_A7, 0x00120000);
	C68k_Set_Reg(&C68K, C68K_PC, pc);
	BusErrFlag = 0;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* run about n 68000 cycles in slices of 200 */
static void exec_cycles(long long n)
{
	while (n > 0) {
		C68K.ICount = m68000_ICountBk = 0;
		n -= C68k_Exec(&C68K, 200);
	}
}

static double secs = 0.3;
static FILE *out;		/* results; stdout gets mem_wrap.c's AdrError lines */

/* us per 1M cycles of the program set up at pc */
static void measure(const char *name, DWORD pc)
{
	long long total = 0;
	double t0, t;
	unsigned dev0;

	double best = 0;
	int w;

	exec_cycles(2000000);		/* warm-up */
	dev0 = devacc;
	for (w = 0; w < 3; w++) {	/* the best of 3 windows: interference only slows down */
		total = 0;
		t0 = now();
		do {
			exec_cycles(1000000);
			total += 1000000;
			t = now() - t0;
		} while (t < secs / 3);
		if (!w || t * 1e12 / total < best)
			best = t * 1e12 / total;
	}
	fprintf(out, "%s %.1f\n", name, best);
	if (C68k_Get_Reg(&C68K, C68K_PC) == TRAPADR)
		fprintf(stderr, "%s: ended in an exception\n", name);
	if (getenv("BENCH_DEBUG"))
		fprintf(stderr, "%s: %u device accesses, %ld bytes of AdrError lines\n", name, devacc - dev0, ftell(stdout));
	fflush(out);
	(void)pc; (void)dev0;
}

/* ---- simple loops (as the PSP cpubench) ---- */

#define CODE	0x00080000

static void t_reg(void)
{
	static const WORD p[] = {	/* registers only */
		0x7e00,		/* L: moveq #0,d7 */
		0x7203,		/*    moveq #3,d1 */
		0xd481,		/*    add.l d1,d2 */
		0xe58b,		/*    lsl.l #2,d3 */
		0xb543,		/*    eor.w d2,d3 */
		0x5244,		/*    addq.w #1,d4 */
		0xba44,		/*    cmp.w d4,d5 */
		0x6702,		/*    beq.s +2 */
		0x3c03,		/*    move.w d3,d6 */
		0x51c8, 0xffec,	/*    dbra d0,L */
		0x60e8,		/*    bra.s L */
	};
	reset_cpu(CODE);
	put(CODE, p, sizeof(p) / 2);
	C68K.D[0] = 1000;
	C68K.D[5] = 77;
	measure("reg", CODE);
}

static void t_ram(void)
{
	static const WORD p[] = {	/* typical RAM traffic */
		0x41f9, 0x0008, 0x1000,	/* S: lea $81000,a0 */
		0x43f9, 0x0008, 0x1400,	/*    lea $81400,a1 */
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
	reset_cpu(CODE);
	put(CODE, p, sizeof(p) / 2);
	C68K.A[6] = CODE + 0x1c00;
	measure("ram", CODE);
}

/* GPIP read in a loop that is not an idle loop (bra.s) */
static void t_gpip_read(void)
{
	static const WORD p[] = {
		0x0839, 0x0007, 0x00e8, 0x8001,	/* L: btst #7,$e88001 */
		0x60f6,				/*    bra.s L */
	};
	reset_cpu(CODE);
	put(CODE, p, sizeof(p) / 2);
	measure("gpip_read", CODE);
}

/* idle loops: the bit / the RAM word never changes */
static void t_idle(void)
{
	static const WORD gp[] = {
		0x0839, 0x0004, 0x00e8, 0x8001,	/* L: btst #4,$e88001 */
		0x67f6,				/*    beq.s L */
	};
	static const WORD gd[] = {
		0x41f9, 0x00e8, 0x8001,		/*    lea $e88001,a0 */
		0x7204,				/*    moveq #4,d1 */
		0x0310,				/* L: btst d1,(a0) */
		0x67fc,				/*    beq.s L */
	};
	static const WORD tw[] = {
		0x4bf9, 0x0008, 0x1000,		/*    lea $81000,a5 */
		0x4a6d, 0x0010,			/* L: tst.w 16(a5) */
		0x67fa,				/*    beq.s L */
	};
	reset_cpu(CODE);
	put(CODE, gp, sizeof(gp) / 2);
	measure("idle_gpip_abs", CODE);
	reset_cpu(CODE);
	put(CODE, gd, sizeof(gd) / 2);
	measure("idle_gpip_dn", CODE);
	reset_cpu(CODE);
	put(CODE, tw, sizeof(tw) / 2);
	w16(0x81010, 0);
	measure("idle_tst", CODE);
}

/* ---- GVRAM fills (SION IV's 3D drawing: word and movem.l -(An) writes) ---- */

static void t_gvram(void)
{
	static const WORD wl[] = {	/* move.l d0,(a0)+ x4 over 64 KB */
		0x41f9, 0x00c0, 0x0000,	/* S: lea $c00000,a0 */
		0x323c, 0x0fff,		/*    move.w #4095,d1 */
		0x20c0, 0x20c0, 0x20c0, 0x20c0,	/* L: move.l d0,(a0)+ x4 */
		0x51c9, 0xfff6,		/*    dbra d1,L */
		0x5280,			/*    addq.l #1,d0 */
		0x60e6,			/*    bra.s S */
	};
	static const WORD ww[] = {	/* move.w d0,(a0)+ x4 */
		0x41f9, 0x00c0, 0x0000,	/* S: lea $c00000,a0 */
		0x323c, 0x0fff,		/*    move.w #4095,d1 */
		0x30c0, 0x30c0, 0x30c0, 0x30c0,	/* L: move.w d0,(a0)+ x4 */
		0x51c9, 0xfff6,		/*    dbra d1,L */
		0x5280,			/*    addq.l #1,d0 */
		0x60e6,			/*    bra.s S */
	};
	static const WORD mm[] = {	/* movem.l d0-d7,-(a0) */
		0x41f9, 0x00c1, 0x0000,	/* S: lea $c10000,a0 */
		0x323c, 0x07ff,		/*    move.w #2047,d1 */
		0x48e0, 0xff00,		/* L: movem.l d0-d7,-(a0) */
		0x51c9, 0xfffa,		/*    dbra d1,L */
		0x5280,			/*    addq.l #1,d0 */
		0x60ea,			/*    bra.s S */
	};
	CRTC_Regs[0x28] = 3;		/* 65536 colours */
	reset_cpu(CODE);
	put(CODE, wl, sizeof(wl) / 2);
	measure("gvram_long64k", CODE);
	reset_cpu(CODE);
	put(CODE, mm, sizeof(mm) / 2);
	measure("gvram_movem64k", CODE);
	reset_cpu(CODE);
	put(CODE, ww, sizeof(ww) / 2);
	measure("gvram_word64k", CODE);
	CRTC_Regs[0x28] = 0;		/* 16 colours, 512 dots */
	reset_cpu(CODE);
	put(CODE, ww, sizeof(ww) / 2);
	measure("gvram_word16", CODE);
}

/* ---- instruction mix ---- */

/*
 * A straight-line block of MIX_N instructions drawn from the handlers of
 * tools/c68ktest/hot995.txt (host profiles of SION IV, 超連射68K and
 * Gradius, hottest first), handler of rank r with weight 1 / (r + 1): the
 * list has no counts, only the order.  The block starts by loading every
 * register (addresses into a scratch area), then the instructions follow
 * with fixed extension words (displacements, indexes on An, absolute
 * addresses all in the scratch area, non-zero immediates).  Branches,
 * BSR/JSR/RTS and DBcc are emitted as small patterns that end at the next
 * instruction whether taken or not.  Handlers that cannot run in a straight
 * line (exceptions, SR/USP writes, JMP/JSR through registers, LINK/UNLK,
 * DIV by a register) are left out.  Each instruction is checked by running
 * the block step by step: it must not raise an exception, leave the
 * scratch area or touch a device; one that does is replaced.
 */
#include "mixops.h"

#define MIX_CODE	0x4000		/* reachable with abs.w too */
#define MIX_N		1024
#define SCR_LO		0x000d0000	/* every address the block may use */
#define SCR_HI		0x00150000
#define NH		(int)(sizeof(mix_handlers) / sizeof(mix_handlers[0]))

static DWORD mrs = 12345;
static DWORD mrnd(void) { mrs ^= mrs << 13; mrs ^= mrs >> 17; mrs ^= mrs << 5; return mrs; }

static const DWORD dinit[8] = { 0x00000020, 0x12345678, 0x00000003, 0xfffffffe,
				0x0000c010, 0x7fff0001, 0x00000100, 0x80000000 };

static int is_branch(const char *n)
{
	static const char *const cc[] = { "bra", "bhi", "bls", "bcc", "bcs", "bne", "beq", "bvc",
					  "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble" };
	int i;
	for (i = 0; i < 15; i++)
		if (!strncmp(n, cc[i], 3) && (!strcmp(n + 3, "_8") || !strcmp(n + 3, "_16")))
			return 1;
	return 0;
}

/* 0: left out, 1: plain, 2: pattern */
static int mix_kind(const char *n)
{
	static const char *const out[] = { "jmp", "rte", "rtr", "trap", "chk", "illegal", "stop",
					   "reset", "link", "unlk", "1010", "1111", "jsr", "divs",
					   "divu", "bsr", "rts", "db", NULL };
	int i;
	if (is_branch(n) || !strcmp(n, "bsr_8") || !strcmp(n, "bsr_16") ||
	    !strcmp(n, "jsr_32_al") || !strcmp(n, "rts_32") || !strncmp(n, "db", 2))
		return 2;
	if ((!strncmp(n, "divs", 4) || !strncmp(n, "divu", 4)) && !strcmp(n + strlen(n) - 2, "_i"))
		return 1;
	if (strstr(n, "tos") || strstr(n, "usp"))
		return 0;
	for (i = 0; out[i]; i++)
		if (!strncmp(n, out[i], strlen(out[i])))
			return 0;
	return 1;
}

/* the words of one instruction (or pattern) of handler h at address a */
static int mix_emit(int h, DWORD a, WORD *w)
{
	const char *n = mix_handlers[h].name;
	WORD op = mix_handlers[h].op[mrnd() % mix_handlers[h].n];
	DWORD sub;

	if (is_branch(n)) {
		if (n[4] == '8') {			/* Bcc.s *+4; nop */
			w[0] = (op & 0xff00) | 0x02; w[1] = 0x4e71;
			return 2;
		}
		w[0] = op & 0xff00; w[1] = 0x0004; w[2] = 0x4e71;	/* Bcc.w *+6; nop */
		return 3;
	}
	if (!strncmp(n, "db", 2)) {			/* DBcc Dn,*+6; nop */
		w[0] = op; w[1] = 0x0004; w[2] = 0x4e71;
		return 3;
	}
	if (!strcmp(n, "bsr_8") || !strcmp(n, "rts_32")) {	/* bsr.s S; bra.s N; S: rts; N: */
		w[0] = 0x6102; w[1] = 0x6002; w[2] = 0x4e75;
		return 3;
	}
	if (!strcmp(n, "bsr_16")) {			/* bsr.w S; bra.s N; S: rts; N: */
		w[0] = 0x6100; w[1] = 0x0004; w[2] = 0x6002; w[3] = 0x4e75;
		return 4;
	}
	if (!strcmp(n, "jsr_32_al")) {			/* jsr S.l; bra.s N; S: rts; N: */
		sub = a + 8;
		w[0] = 0x4eb9; w[1] = sub >> 16; w[2] = sub; w[3] = 0x6002; w[4] = 0x4e75;
		return 5;
	}
	/*
	 * Extension words: as d16 small, as index (d8,An,Xn) A0/A4 (low words
	 * of the scratch addresses), as abs.l in the scratch area, as abs.w
	 * the ROM (reads only; a write is a bus error and is replaced), as
	 * immediates non-zero.
	 */
	w[0] = op;
	w[1] = 0x8010; w[2] = 0xc010; w[3] = 0x8010; w[4] = 0xc010; w[5] = 0x8010;
	if (!strncmp(n, "movem", 5))
		w[1] = 0x0030;				/* d4-d5 (or a2-a3 for -(An)) */
	return 6;	/* the real length is found by running it */
}

static BYTE *scr_save, *low_save;

static void mix_preamble(DWORD *a)
{
	int k;
	for (k = 0; k < 8; k++) {			/* move.l #x,Dk */
		w16(*a, 0x203c | (k << 9)); w32(*a + 2, dinit[k]); *a += 6;
	}
	for (k = 0; k < 8; k++) {			/* lea x,Ak */
		w16(*a, 0x41f9 | (k << 9)); w32(*a + 2, 0x00100000 + k * 0x2000 + (k == 7 ? 0x1000 : 0)); *a += 6;
	}
}

static int regs_ok(void)
{
	int k;
	for (k = 0; k < 8; k++)
		if ((C68K.A[k] & 0xffffff) < SCR_LO || (C68K.A[k] & 0xffffff) >= SCR_HI)
			return 0;
	return 1;
}

/* one step from the current state; 0 if it misbehaved */
static int step_ok(DWORD lo, DWORD hi)
{
	unsigned d = devacc;
	DWORD pc;
	int odd;

	C68K.ICount = m68000_ICountBk = 0;
	C68k_Exec(&C68K, 1);
	pc = C68k_Get_Reg(&C68K, C68K_PC);
	odd = ftell(stdout) > 0;	/* AdrError: a word access at an odd address */
	rewind(stdout);
	return !odd && devacc == d && pc >= lo && pc <= hi && pc != TRAPADR && !(BusErrFlag & 3) && regs_ok();
}

static int mix_n, mix_cover, mix_handled[NH];
static double mix_wcover;
static WORD mix_ban[256][2];	/* handler, opcode found to misbehave in later runs */
static int mix_nban;

static int banned(int h, WORD op)
{
	int k;
	for (k = 0; k < mix_nban; k++)
		if (mix_ban[k][0] == h && mix_ban[k][1] == op)
			return 1;
	return 0;
}

/* build the block; -1 if it stays well behaved, else the instruction that did not */
static int build_once(int *pick)
{
	double wsum = 0, wt[NH], r;
	DWORD a, start[MIX_N + 1], body, pc, prev;
	WORD w[8], ops[MIX_N];
	int h, i, k, tries, it;

	for (h = 0; h < NH; h++) {
		wt[h] = mix_kind(mix_handlers[h].name) ? 1.0 / (h + 1) : 0;
		wsum += wt[h];
	}
	mrs = 12345;
	for (i = SCR_LO; i < SCR_HI; i += 2)
		w16(i, mrnd());
	reset_cpu(MIX_CODE);
	a = MIX_CODE;
	mix_preamble(&a);
	body = a;
	while (C68k_Get_Reg(&C68K, C68K_PC) < body)	/* run the preamble */
		step_ok(MIX_CODE, body);
	for (i = 0; i < MIX_N; i++) {
		start[i] = a;
		for (tries = 0; ; tries++) {
			c68k_struc cpu = C68K;
			int n;

			if (tries > 1000) { fprintf(stderr, "mix: cannot place instruction %d\n", i); exit(1); }
			r = (mrnd() / 4294967296.0) * wsum;
			for (h = 0; h < NH - 1 && (r -= wt[h]) > 0; h++)
				;
			if (!wt[h])
				continue;
			n = mix_emit(h, a, w);
			if (banned(h, w[0]))
				continue;
			memcpy(scr_save, MEM + SCR_LO, SCR_HI - SCR_LO);
			memcpy(low_save, MEM, 0x10000);
			put(a, w, n);
			/* run it: a pattern to its end, an instruction one step */
			k = 1;
			do {
				if (!step_ok(MIX_CODE, a + 2 * n)) { k = 0; break; }
				pc = C68k_Get_Reg(&C68K, C68K_PC);
			} while (mix_kind(mix_handlers[h].name) == 2 && pc != a + 2 * n);
			if (k && mix_kind(mix_handlers[h].name) == 1) {
				n = (pc - a) / 2;	/* the instruction's real length */
				k = pc > a && n <= 5;
			}
			if (k) {
				pick[i] = h;
				ops[i] = w[0];
				a += 2 * n;
				break;
			}
			C68K = cpu;
			memcpy(MEM + SCR_LO, scr_save, SCR_HI - SCR_LO);
			memcpy(MEM, low_save, 0x10000);
		}
	}
	start[MIX_N] = a;
	w16(a, 0x6000); w16(a + 2, (WORD)(MIX_CODE - (a + 2)));	/* bra.w to the preamble */
	/* the whole block, several times over: it must stay well behaved */
	for (it = 0; it < 64; it++) {
		for (;;) {
			prev = C68k_Get_Reg(&C68K, C68K_PC);
			if (prev == a)
				break;
			if (!step_ok(MIX_CODE, a)) {
				for (i = 0; i < MIX_N && !(prev >= start[i] && prev < start[i + 1]); i++)
					;
				if (i == MIX_N || mix_nban == 256) {
					fprintf(stderr, "mix: block misbehaves at %06x\n", prev);
					exit(1);
				}
				mix_ban[mix_nban][0] = pick[i];
				mix_ban[mix_nban++][1] = ops[i];
				return i;
			}
		}
		step_ok(MIX_CODE, a + 4);
	}
	return -1;
}

static void build_mix(void)
{
	static int pick[MIX_N];
	double r;
	int h, i;

	scr_save = malloc(SCR_HI - SCR_LO);
	low_save = malloc(0x10000);
	while (build_once(pick) >= 0)
		;
	mix_n = MIX_N;
	for (i = 0; i < MIX_N; i++)
		mix_handled[pick[i]]++;
	for (h = 0; h < NH; h++)
		if (mix_handled[h]) mix_cover++;
	for (h = 0, r = 0; h < NH; h++) {
		if (mix_kind(mix_handlers[h].name)) mix_wcover += 1.0 / (h + 1);
		r += 1.0 / (h + 1);
	}
	mix_wcover /= r;
}

static void t_mix(void)
{
	build_mix();
	fprintf(stderr, "mix: %d instructions, %d of %d handlers of the list used, "
		"%.0f%% of the list's weight usable, %d replaced after the first run\n",
		mix_n, mix_cover, NH, mix_wcover * 100, mix_nban);
	{	/* the block must be the same in every variant */
		DWORD a, h = 2166136261u;
		for (a = MIX_CODE; a < MIX_CODE + 0x3000; a++)
			h = (h ^ MEM[a]) * 16777619u;
		fprintf(out, "mix_hash %08x\n", h);
	}
	reset_cpu(MIX_CODE);
	measure("mix", MIX_CODE);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		secs = atof(argv[1]);
	out = fdopen(dup(1), "w");
	stdout = tmpfile();
	IPL = calloc(1, 0x40000);
	MEM = calloc(1, 0xc00000);
	FONT = calloc(1, 0xc0000);
	m68000_init();
	t_reg();
	t_ram();
	t_gpip_read();
	t_idle();
	t_gvram();
	t_mix();
	return 0;
}
