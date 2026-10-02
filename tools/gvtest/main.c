#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "stub.h"

BYTE gv_store[PAD + 0x80000 + PAD] __attribute__((aligned(64)));
WORD pal_store[65536 + 1024] __attribute__((aligned(64)));
WORD Grp_LineBuf[1024] __attribute__((aligned(64)));
WORD Grp_LineBufSP[1024] __attribute__((aligned(64)));
WORD Grp_LineBufSP2[1024] __attribute__((aligned(64)));
WORD Pal16Adr[256];
BYTE Pal_Regs[1024];
WORD Pal16[65536];
WORD Ibit, Pal_HalfMask, Pal_Ix2;
BYTE CRTC_Regs[48];
DWORD TextDotX, TextDotY;
DWORD GrphScrollX[4], GrphScrollY[4];
DWORD VLINE;
BYTE TextDirtyLine[1024];
WORD CRTC_FastClrMask;

#define DECL(p) \
	void p##Grp_DrawLine16(void); void p##Grp_DrawLine8(int, int); void p##Grp_DrawLine4(DWORD, int); \
	void p##Grp_DrawLine4h(void); void p##Grp_DrawLine16SP(void); void p##Grp_DrawLine8SP(int); \
	void p##Grp_DrawLine4SP(DWORD); void p##Grp_DrawLine4hSP(void); void p##Grp_DrawLine8TR(int, int); \
	void p##Grp_DrawLine4TR(DWORD, int);
DECL(old_)
DECL(new_)
void new_Grp_DrawLine4Multi(DWORD, int);
static DWORD m_pages; static int m_n;

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

static void call(int fn, int ver, int page, int opaq)
{
	switch (fn) {
	case 0: ver ? new_Grp_DrawLine16() : old_Grp_DrawLine16(); break;
	case 1: ver ? new_Grp_DrawLine8(page, opaq) : old_Grp_DrawLine8(page, opaq); break;
	case 2: ver ? new_Grp_DrawLine4(page, opaq) : old_Grp_DrawLine4(page, opaq); break;
	case 3: ver ? new_Grp_DrawLine4h() : old_Grp_DrawLine4h(); break;
	case 4: ver ? new_Grp_DrawLine16SP() : old_Grp_DrawLine16SP(); break;
	case 5: ver ? new_Grp_DrawLine8SP(page) : old_Grp_DrawLine8SP(page); break;
	case 6: ver ? new_Grp_DrawLine4SP(page) : old_Grp_DrawLine4SP(page); break;
	case 7: ver ? new_Grp_DrawLine4hSP() : old_Grp_DrawLine4hSP(); break;
	case 8: ver ? new_Grp_DrawLine8TR(page, opaq) : old_Grp_DrawLine8TR(page, opaq); break;
	case 9: ver ? new_Grp_DrawLine4TR(page, opaq) : old_Grp_DrawLine4TR(page, opaq); break;
	case 10:
		if (ver) new_Grp_DrawLine4Multi(m_pages, m_n);
		else { int k; for (k = 0; k < m_n; k++) old_Grp_DrawLine4((m_pages >> (k * 2)) & 3, k == 0); }
		break;
	}
}
static const char *names[11] = {"16", "8", "4", "4h", "16SP", "8SP", "4SP", "4hSP", "8TR", "4TR", "4M"};

static void fill_gvram(int mode)
{
	size_t i;
	for (i = 0; i < sizeof(gv_store); i++) {
		BYTE b = rnd();
		if (mode == 1) { if (rnd() % 4) b &= 0x0f; if (rnd() % 4) b &= 0xf0; }	/* sparse nibbles */
		else if (mode == 2) { if (rnd() % 16) b = 0; }
		gv_store[i] = b;
	}
}

static WORD s0[3][1024], r0[3][1024];

int main(int argc, char **argv)
{
	long cases = argc > 1 ? atol(argv[1]) : 2000000;
	long c, fails = 0, per[11] = {0};
	int i;
	static const DWORD widths[] = {256, 384, 512, 768, 1024, 255, 257, 511, 513, 1, 2, 3};

	for (i = 0; i < 128; i++) { Pal16Adr[i*2] = i*4; Pal16Adr[i*2+1] = i*4+1; }
	for (c = 0; c < cases; c++) {
		int fn = rnd() % 11, page = rnd() % 8, opaq = (rnd() % 3 == 0) ? 0 : (rnd() % 3);
		if (c % 20000 == 0) {
			fill_gvram(c / 20000 % 3);
			for (i = 0; i < 65536 + 1024; i++) pal_store[i] = rnd();
			if (rnd() & 1) for (i = 0; i < 16; i++) if (rnd() % 4 == 0) pal_store[i] = 0;
			for (i = 0; i < 1024; i++) Pal_Regs[i] = rnd();
			for (i = 0; i < 65536; i++) Pal16[i] = rnd();
		}
		if (rnd() % 4 == 0) pal_store[rnd() % 16] = (rnd() & 1) ? 0 : rnd();
		Ibit = rnd(); Pal_HalfMask = rnd(); Pal_Ix2 = rnd();
		for (i = 0; i < 48; i++) CRTC_Regs[i] = rnd();
		TextDotX = (rnd() % 3) ? widths[rnd() % 12] : 1 + rnd() % 1024;
		VLINE = rnd() % ((rnd() & 1) ? 1024 : 600);
		for (i = 0; i < 4; i++) {
			GrphScrollX[i] = (rnd() % 4 == 0) ? rnd() : rnd() % 1024;
			GrphScrollY[i] = (rnd() % 4 == 0) ? rnd() : rnd() % 1024;
			if (rnd() % 8 == 0) GrphScrollX[i] = 511 - rnd() % 3;
			if (rnd() % 8 == 0) GrphScrollX[i] &= ~1u;
		}
		m_pages = rnd() & 0xff; m_n = 1 + rnd() % 4;
		if (rnd() & 1) {	/* descending pages (VCReg1 = e4), own scroll each */
			int p; m_pages = 0; m_n = 0;
			for (p = 3; p >= 0; p--)
				if (rnd() % 4) { m_pages |= (DWORD)p << (2 * m_n); m_n++; }
			if (!m_n) { m_pages = 3; m_n = 1; }
		}
		if (fn == 10 && rnd() % 3 == 0) {	/* mostly all pages at one place */
			for (i = 1; i < 4; i++) { GrphScrollX[i] = GrphScrollX[0] + (rnd() % 8 ? 0 : 0x200); GrphScrollY[i] = GrphScrollY[0] + (rnd() % 8 ? 0 : 0x200); }
		}
		for (i = 0; i < 1024; i++) {
			s0[0][i] = rnd(); s0[1][i] = (rnd() & 1) ? 0 : rnd(); s0[2][i] = rnd();
		}
		memcpy(Grp_LineBuf, s0[0], 2048); memcpy(Grp_LineBufSP, s0[1], 2048); memcpy(Grp_LineBufSP2, s0[2], 2048);
		call(fn, 0, page, opaq);
		memcpy(r0[0], Grp_LineBuf, 2048); memcpy(r0[1], Grp_LineBufSP, 2048); memcpy(r0[2], Grp_LineBufSP2, 2048);
		memcpy(Grp_LineBuf, s0[0], 2048); memcpy(Grp_LineBufSP, s0[1], 2048); memcpy(Grp_LineBufSP2, s0[2], 2048);
		call(fn, 1, page, opaq);
		per[fn]++;
		if (memcmp(r0[0], Grp_LineBuf, 2048) || memcmp(r0[1], Grp_LineBufSP, 2048) || memcmp(r0[2], Grp_LineBufSP2, 2048)) {
			if (fails++ < 10)
				printf("MISMATCH case %ld fn %s page %d opaq %d dotx %u vline %u sx %x sy %x r29 %02x\n",
				       c, names[fn], page, opaq, TextDotX, VLINE, GrphScrollX[page&3], GrphScrollY[page&3], CRTC_Regs[0x29]);
		}
	}
	for (i = 0; i < 11; i++) printf("%s:%ld ", names[i], per[i]);
	printf("\ncases %ld, mismatches %ld\n", cases, fails);

	/* benchmark: 256 dots, 256 lines, realistic data */
	if (argc > 2) {
		int fn, ver, rep;
		fill_gvram(1);
		for (i = 0; i < 256; i++) pal_store[i] = rnd() | 1;
		pal_store[0] = 0;
		TextDotX = 256; CRTC_Regs[0x29] = 0;
		for (fn = 0; fn < 11; fn++) {
			double t[2];
			for (ver = 0; ver < 2; ver++) {
				struct timespec a, b;
				clock_gettime(CLOCK_MONOTONIC, &a);
				for (rep = 0; rep < 200; rep++) {
					GrphScrollX[0] = GrphScrollX[1] = GrphScrollX[2] = GrphScrollX[3] = rep * 3;
					for (VLINE = 0; VLINE < 256; VLINE++) {
						int p;
						if (fn == 10) { m_pages = 0xe4; m_n = 4; call(fn, ver, 0, 0); }
						else for (p = 0; p < 4; p++) {
							memset(Grp_LineBufSP, p & 1, 2);
							call(fn, ver, p, p == 0);
						}
					}
				}
				clock_gettime(CLOCK_MONOTONIC, &b);
				t[ver] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
			}
			printf("%-5s old %8.2f ms new %8.2f ms  x%.2f\n", names[fn], t[0], t[1], t[0] / t[1]);
		}
	}
	return fails != 0;
}
