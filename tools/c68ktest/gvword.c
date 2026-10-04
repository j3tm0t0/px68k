/* gvword.sh: GVRAM_WriteWord == GVRAM_Write(adr, hi) + GVRAM_Write(adr + 1, lo), real gvram.c.
 * GE hooks (-DPSP): same guard addresses called (as a set), same words reported
 * changed (GE_G16Write) / same rows whose generation moved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
extern BYTE GVRAM[0x80000];
BYTE CRTC_Regs[48];
DWORD GrphScrollY[4];
BYTE TextDirtyLine[1024];
#ifdef PSP
DWORD GE_GRowGen[512], GE_TRowGen[1024], GE_GGenAll, GE_TGenAll;
volatile int GE_Guard;
int GE_G16Live;
static BYTE wchg[0x40000];	/* words reported by GE_G16Write */
static DWORD guards[8]; static int nguard;
void GE_GvramGuard(DWORD a) { if (nguard < 8) guards[nguard++] = a; }
void GE_G16Write(DWORD a) { wchg[(a & 0x7ffff) >> 1] = 1; }
#endif
void GVRAM_Write(DWORD adr, BYTE data);
void GVRAM_WriteWord(DWORD adr, WORD data);

static unsigned rs = 12345;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static BYTE g0[0x80000], t0[1024], g1[0x80000], t1[1024];
#ifdef PSP
static DWORD r0[512], r1[512];
static BYTE w1[0x40000];
static DWORD gd1[8]; static int ng1;
#endif

int main(void)
{
	static const BYTE modes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xb, 0xc, 0x13, 0x10 };
	int i, k, bad = 0;
	for (i = 0; i < 0x80000; i++) GVRAM[i] = rnd();
	for (i = 0; i < 150000; i++) {
		DWORD adr = 0xc00000 + (rnd() & 0x1ffffe);
		WORD d = rnd();
		CRTC_Regs[0x28] = modes[rnd() % sizeof(modes)];
		for (k = 0; k < 4; k++) GrphScrollY[k] = rnd() % 4 ? rnd() & 1023 : rnd();
		if (rnd() % 2) adr &= 0xc7fffe;
		if (rnd() % 4 == 0) d = *(WORD *)&GVRAM[(adr - 0xc00000) & 0x7fffe];	/* unchanged */
		else if (rnd() % 4 == 0) d = (*(WORD *)&GVRAM[(adr - 0xc00000) & 0x7fffe] & 0xff00) | (d & 0xff);
#ifdef PSP
		GE_Guard = rnd() % 2;
		GE_G16Live = rnd() % 2;
		memcpy(r0, GE_GRowGen, sizeof r0);
		memset(wchg, 0, sizeof wchg); nguard = 0;
#endif
		memcpy(g0, GVRAM, sizeof g0); memcpy(t0, TextDirtyLine, sizeof t0);
		GVRAM_Write(adr, d >> 8);
		GVRAM_Write(adr + 1, d & 0xff);
		memcpy(g1, GVRAM, sizeof g1); memcpy(t1, TextDirtyLine, sizeof t1);
#ifdef PSP
		memcpy(r1, GE_GRowGen, sizeof r1); memcpy(GE_GRowGen, r0, sizeof r0);
		memcpy(w1, wchg, sizeof w1); memset(wchg, 0, sizeof wchg);
		memcpy(gd1, guards, sizeof gd1); ng1 = nguard; nguard = 0;
#endif
		memcpy(GVRAM, g0, sizeof g0); memcpy(TextDirtyLine, t0, sizeof t0);
		GVRAM_WriteWord(adr, d);
		{
			int diff = memcmp(g1, GVRAM, sizeof g1) || memcmp(t1, TextDirtyLine, sizeof t1);
#ifdef PSP
			for (k = 0; k < 512; k++)
				if ((r1[k] != r0[k]) != (GE_GRowGen[k] != r0[k])) diff |= 2;
			if (memcmp(w1, wchg, sizeof w1)) diff |= 4;
			/* guard: the same calls, or one call with an address of the byte path */
			if (!(nguard == ng1 && !memcmp(gd1, guards, nguard * sizeof(DWORD))) &&
			    !(nguard == 1 && ng1 >= 1 && (guards[0] == gd1[0] || (ng1 > 1 && guards[0] == gd1[1])))) diff |= 8;
#endif
			if (diff && bad++ < 10)
				printf("DIFF %d adr %06x data %04x r28 %02x\n", diff, adr, d, CRTC_Regs[0x28]);
		}
		if (rnd() % 64 == 0) memset(TextDirtyLine, 0, sizeof TextDirtyLine);
	}
	printf("%d differences in %d writes\n", bad, i);
	return bad != 0;
}
