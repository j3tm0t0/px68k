/* Differential test harness for the C68K core + mem_wrap.c.
 * Links the real c68k.c / m68000.c / mem_wrap.c with stub devices that log
 * every access into a running hash.  Two builds (old / new sources) must
 * print identical output for the same arguments.
 *
 *   harness fuzz <ncases> <seed> [first last]	random instructions and state
 *   harness idle <ncases> <seed> [first last]	the same plus generated
 *						"tst/cmp (d16,An); bcc.s *-4" loops
 *   harness ipl  <iplrom> <nslices>		run an IPL ROM on the stubs
 *
 * Results go to stderr ("final <hash>"); [first last] also dumps those
 * cases.  Built and compared by fuzz.sh (32-bit host: the core keeps host
 * pointers in UINT32).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c68k.h"

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;

extern BYTE *IPL, *MEM, *FONT;
/* a whole 64KB fetch bank each: random jumps may run past the real size */
BYTE SCSIIPL[0x10000];
BYTE SRAM[0x10000];
BYTE GVRAM[0x80000];
BYTE TVRAM[0x80000];
extern int ICount;
int m68000_ICountBk_unused;
extern DWORD BusErrFlag, BusErrAdr, BusErrHandling, MemByteAccess;
extern int m68000_ICountBk;
void m68000_init(void);

static DWORD H = 2166136261u;
static DWORD accn;
static void mix(DWORD v) { H = (H ^ v) * 16777619u; }

static DWORD rs;
static DWORD rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* device stubs: deterministic values depending on address and access count */
static BYTE devread(int id, DWORD adr)
{
	accn++;
	mix(0x100 + id); mix(adr); mix(accn);
	return (BYTE)((adr * 2654435761u + accn * 40503u + id) >> 13);
}
static void irq_like(void)
{
	/* what IRQH_Int does: raise an interrupt and end the slice */
	C68k_Set_IRQ(&C68K, 1 + (accn % 7), 2 /* HOLD_LINE */);
	if (C68K.ICount) {
		m68000_ICountBk += C68K.ICount;
		C68K.ICount = 0;
	}
}
static void devwrite(int id, DWORD adr, BYTE v)
{
	accn++;
	mix(0x200 + id); mix(adr); mix(v); mix(accn);
	if (id == 5 && (v & 0x0f) == 3) irq_like();
}

#define DEV(name, id) \
	BYTE name##_Read(DWORD a) { return devread(id, a); } \
	void name##_Write(DWORD a, BYTE v) { devwrite(id, a, v); }
DEV(ADPCM, 1) DEV(BG, 2) DEV(CRTC, 3) DEV(DMA, 4) DEV(FDC, 6)
DEV(IOC, 7) DEV(Mcry, 8) DEV(MIDI, 9) DEV(Pal, 10) DEV(PIA, 11) DEV(RTC, 12)
DEV(SASI, 13) DEV(SCC, 14) DEV(SCSI, 15) DEV(SRAM, 16) DEV(SysPort, 17)
DEV(VCtrl, 18)
/* GPIP ($e88001) has no side effect in mfp.c and only changes between slices */
static BYTE gpip;
BYTE MFP_Read(DWORD a) { return a == 0xe88001 ? gpip : devread(5, a); }
void MFP_Write(DWORD a, BYTE v) { devwrite(5, a, v); }
/* TVRAM / GVRAM behave like memory (plus logging of writes) */
BYTE TVRAM_Read(DWORD a) { a &= 0x7ffff; a ^= 1; return TVRAM[a]; }
void TVRAM_Write(DWORD a, BYTE v) { devwrite(19, a, v); TVRAM[(a & 0x7ffff) ^ 1] = v; }
BYTE GVRAM_Read(DWORD a) { mix(0x300); mix(a); return GVRAM[(a & 0x7ffff) ^ 1]; }
void GVRAM_Write(DWORD a, BYTE v) { devwrite(20, a, v); GVRAM[(a & 0x7ffff) ^ 1] = v; }
BYTE OPM_Read(WORD a) { return devread(21, a); }
void OPM_Write(DWORD r, BYTE v) { devwrite(22, r, v); }

static int irqcb(int line) { mix(0x400 + line); return (accn & 3) == 0 ? -1 : 24 + line; }

static void hash_state(INT32 ret)
{
	int i;
	for (i = 0; i < 8; i++) { mix(C68K.D[i]); mix(C68K.A[i]); }
	mix(C68K.flag_C); mix(C68K.flag_V); mix(C68K.flag_Z); mix(C68K.flag_N);
	mix(C68K.flag_X); mix(C68K.flag_I); mix(C68K.flag_S); mix(C68K.USP);
	mix(C68k_Get_Reg(&C68K, C68K_PC)); mix(C68K.HaltState); mix(C68K.IRQLine);
	mix(C68K.IRQState); mix(C68K.ICount); mix(ret);
	mix(BusErrFlag); mix(BusErrAdr); mix(BusErrHandling); mix(MemByteAccess);
	mix(m68000_ICountBk);
}

static DWORD memhash(BYTE *p, int n)
{
	DWORD h = 2166136261u; int i;
	for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

static DWORD rand_adr(void)
{
	DWORD r = rnd(), a;
	switch (rnd() % 10) {
	case 0: case 1: case 2: case 3:
		a = (r & 0x1ffff) | (rnd() & 0xff000000); break;	/* RAM window */
	case 4: a = 0xe80000 + (r & 0x7ffff); break;		/* I/O */
	case 5: a = 0xc00000 + (r & 0x7ffff); break;		/* GVRAM */
	case 6: a = 0xe00000 + (r & 0x7ffff); break;		/* TVRAM */
	case 7: a = 0xa00000 + (r & 0x1fffff); break;		/* bus error */
	case 8: a = 0x9ffff0 + (r & 0x1f); break;		/* RAM end */
	default: a = 0xfc0000 + (r & 0x3ffff); break;		/* ROM */
	}
	if (rnd() % 8 == 0) a |= 1; else if (rnd() % 2) a &= ~1;
	return a;
}

int main(int argc, char **argv)
{
	int i, n;
	INT32 ret;

	IPL = calloc(1, 0x40000);
	MEM = calloc(1, 0xc00000);
	FONT = calloc(1, 0xc0000);
	m68000_init();
	C68k_Set_IRQ_Callback(&C68K, irqcb);
	{	/* unmapped fetch banks -> 16MB junk (the real core would run off its 64KB bad_address buffer) */
		BYTE *junk = calloc(1, 0x1000000);
		DWORD bad = C68K.Fetch[0x80 >> 0 & 0xff];
		int b;
		bad = C68K.Fetch[0xe9];
		for (b = 0; b < 256; b++)
			if (C68K.Fetch[b] == bad) C68K.Fetch[b] = (DWORD)junk;
		for (b = 0; b < 0x1000000; b++) junk[b] = (b * 2654435761u) >> 24;
	}

	if (argc >= 4 && (!strcmp(argv[1], "fuzz") || !strcmp(argv[1], "idle"))) {
		int idle = !strcmp(argv[1], "idle");
		n = atoi(argv[2]);
		rs = atoi(argv[3]) * 2654435761u + 1;
		for (i = 0; i < 0x40000; i++) IPL[i] = rnd();
		for (i = 0; i < 0x20000; i++) MEM[i] = rnd();
		for (i = 0; i < 0x80000; i++) { GVRAM[i] = rnd(); TVRAM[i] = rnd(); }
		for (i = 0; i < n; i++) {
			int k;
			/* vectors -> 0x1000 + small offsets (code area) */
			for (k = 0; k < 256; k++) {
				DWORD v = 0x1000 + ((rnd() & 0x3f) << 1);
				if (rnd() % 64 == 0) v = rand_adr();
				MEM[k * 4 + 1] = v >> 24; MEM[k * 4] = v >> 16;
				MEM[k * 4 + 3] = v >> 8; MEM[k * 4 + 2] = v;
			}
			for (k = 0x1000; k < 0x1100; k++) MEM[k] = rnd();
			if (idle) {	/* tst.x d16(An); bcc.s -6 at 0x1010 */
				static const WORD tsts[] = { 0x4a68, 0x4a28, 0xb068, 0xb028, 0x4a50, 0x4a78, 0x4a40, 0x4aa8, 0xb0a8, 0x4268, 0xb168, 0xb0e8 };
				WORD t = tsts[rnd() % 12] | (rnd() & 7), b = 0x60fa | ((rnd() & 15) << 8);
				if (t & 0x8000) t |= (rnd() & 7) << 9;
				if (rnd() % 4 == 0) b = (b & 0xff00) | (rnd() % 3 ? 0xfa : rnd() & 0xff);
				DWORD d = (rnd() % 4) ? (rnd() & 0x7f) : rnd();
				if (rnd() % 8 == 0) t = tsts[rnd() % 4] | (rnd() & 7);
				*(WORD *)(MEM + 0x1010) = t;
				*(WORD *)(MEM + 0x1012) = d;
				*(WORD *)(MEM + 0x1014) = b;
			}
			C68k_Reset(&C68K);
			for (k = 0; k < 8; k++) {
				C68K.D[k] = (rnd() % 3) ? rand_adr() : rnd();
				C68K.A[k] = rand_adr();
			}
			C68K.A[7] = 0x8000 + (rnd() & 0x3ffe);
			C68K.USP = 0xc000 + (rnd() & 0x3ffe);
			if (rnd() % 16 == 0) C68K.A[7] |= 1;
			C68k_Set_Reg(&C68K, C68K_SR, rnd() & 0xa71f);
			C68k_Set_Reg(&C68K, C68K_PC, (rnd() % 32) ? 0x1000 + ((rnd() & 0x3f) << 1) : rand_adr());
			if (idle) {
				C68k_Set_Reg(&C68K, C68K_PC, (rnd() % 3) ? 0x1014 : 0x1010);
				switch (rnd() % 3) {	/* GPIP polls: btst Dn,(An) / btst #n,abs.l; bcc.s back */
				case 0: {
					int r = rnd() & 7;
					*(WORD *)(MEM + 0x1012) = 0x0110 | ((rnd() & 7) << 9) | r;
					*(WORD *)(MEM + 0x1014) = (*(WORD *)(MEM + 0x1014) & 0xff00) | (rnd() % 4 ? 0xfc : 0xfa);
					if (rnd() % 4) C68K.A[r] = 0xe88001 | (rnd() % 4 ? 0 : 0xff000000);
					if (rnd() % 2) C68k_Set_Reg(&C68K, C68K_PC, 0x1012);
					break;
				}
				case 1:
					*(WORD *)(MEM + 0x100c) = 0x0839;
					*(WORD *)(MEM + 0x100e) = rnd() & (rnd() % 4 ? 7 : 0xffff);
					*(WORD *)(MEM + 0x1010) = rnd() % 4 ? 0x00e8 : rnd();
					*(WORD *)(MEM + 0x1012) = rnd() % 4 ? 0x8001 : 0x8000 | (rnd() & 0x3f);
					*(WORD *)(MEM + 0x1014) = (*(WORD *)(MEM + 0x1014) & 0xff00) | (rnd() % 4 ? 0xf6 : 0xfa);
					if (rnd() % 2) C68k_Set_Reg(&C68K, C68K_PC, 0x100c);
					break;
				}
				gpip = rnd();
				if (rnd() % 2) { DWORD a = (C68K.A[MEM[0x1010] & 7] + (INT16)*(WORD *)(MEM + 0x1012)) & 0x1ffff; *(WORD *)(MEM + (a & ~1)) = (rnd() % 2) ? 0 : (rnd() % 2) ? (WORD)C68K.D[(MEM[0x1011] >> 1) & 7] + (rnd() % 3) - 1 : rnd(); }
			}
			if (rnd() % 4 == 0) C68k_Set_IRQ(&C68K, rnd() % 8, rnd() % 4);
			BusErrFlag = (rnd() % 8 == 0) ? (rnd() & 7) : 0;
			BusErrHandling = (rnd() % 32 == 0);
			MemByteAccess = rnd() & 1;
			m68000_ICountBk = 0;
			ret = C68k_Exec(&C68K, 1 + rnd() % (idle ? 400 : 80));
			hash_state(ret);
			if (argc >= 6 && i >= atoi(argv[4]) && i <= atoi(argv[5])) {
				int k;
				fprintf(stderr, "case %d H=%08x ret=%d pc=%06x op=%04x sr=%04x bef=%x beh=%x mba=%x icbk=%d\n", i, H, ret,
					C68k_Get_Reg(&C68K, C68K_PC), *(WORD *)(MEM + 0x1000), C68k_Get_Reg(&C68K, C68K_SR), BusErrFlag, BusErrHandling, MemByteAccess, m68000_ICountBk);
				for (k = 0; k < 8; k++) fprintf(stderr, " D%d=%08x A%d=%08x", k, C68K.D[k], k, C68K.A[k]);
				fprintf(stderr, " mem=%08x\n", memhash(MEM, 0x20000));
			}
			if ((i & 1023) == 1023) {
				mix(memhash(MEM, 0x20000));
				fprintf(stderr, "%d %08x\n", i, H);
			}
		}
		mix(memhash(MEM, 0xc00000));
		mix(memhash(GVRAM, 0x80000));
		mix(memhash(TVRAM, 0x80000));
		fprintf(stderr, "final %08x\n", H);
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "ipl")) {
		FILE *fp = fopen(argv[2], "rb");
		BYTE t;
		if (!fp) return 1;
		fread(&IPL[0x20000], 1, 0x20000, fp);
		fclose(fp);
		memcpy(IPL, &IPL[0x20000], 0x20000);
		for (i = 0; i < 0x40000; i += 2) { t = IPL[i]; IPL[i] = IPL[i + 1]; IPL[i + 1] = t; }
		C68k_Reset(&C68K);
		C68k_Set_Reg(&C68K, C68K_A7, (IPL[0x30001]<<24)|(IPL[0x30000]<<16)|(IPL[0x30003]<<8)|IPL[0x30002]);
		C68k_Set_Reg(&C68K, C68K_PC, (IPL[0x30005]<<24)|(IPL[0x30004]<<16)|(IPL[0x30007]<<8)|IPL[0x30006]);
		n = atoi(argv[3]);
		for (i = 0; i < n; i++) {
			C68K.ICount = m68000_ICountBk = 0;
			if (i % 50 == 0) C68k_Set_IRQ(&C68K, 6, 2);
			C68K.ICount = 200;
			ret = C68k_Exec(&C68K, C68K.ICount);
			hash_state(ret);
			if (i % 10000 == 9999) fprintf(stderr, "%d %08x pc=%06x\n", i, H, C68k_Get_Reg(&C68K, C68K_PC));
		}
		mix(memhash(MEM, 0xc00000));
		fprintf(stderr, "final %08x\n", H);
		return 0;
	}
	fprintf(stderr, "usage\n");
	return 1;
}
