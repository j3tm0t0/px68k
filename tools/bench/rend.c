/*
 * rend.c: speed of the CPU-path line rendering: WinDraw_DrawLine of
 * x11/windraw.c (extracted by wdline.py) with the real graphic (gvram.c),
 * text (tvram.c) and BG/sprite (bg.c) decoders, on screens set up through
 * the emulated registers with synthetic VRAM contents.
 *
 *   rend [seconds per test]	prints "<test> <ns per line>" and
 *				"<test>_hash <hash of the frame>"
 *
 * The hashes let bench.py check that the variants draw the same.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "winx68k.h"
#include "bg.h"
#include "crtc.h"
#include "gvram.h"
#include "palette.h"
#include "tvram.h"

void WinDraw_DrawLine(void);
extern WORD *ScrBuf;
extern BYTE GVRAM[];

/* what the linked files use from the rest of the emulator */
DWORD VLINE, vline;
WORD VLINE_TOTAL = 568;
BYTE Debug_Text = 1, Debug_Grp = 1, Debug_Sp = 1;
int ICount;
DWORD MemByteAccess;
WORD WinDraw_Pal16B = 0x001f, WinDraw_Pal16R = 0xf800, WinDraw_Pal16G = 0x07e0;	/* RGB565 */
void Error(const char *s) { }
void WinDraw_ChangeSize(void) { }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static DWORD rs = 1;
static DWORD rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void crtc(int r, WORD v) { CRTC_Write(0xe80000 + r * 2, v >> 8); CRTC_Write(0xe80000 + r * 2 + 1, v & 0xff); }
static void vctl(DWORD a, WORD v) { VCtrl_Write(a, v >> 8); VCtrl_Write(a + 1, v & 0xff); }
static void bgw(DWORD a, WORD v) { BG_Write(a, v >> 8); BG_Write(a + 1, v & 0xff); }
static void palw(DWORD a, WORD v) { Pal_Write(a, v >> 8); Pal_Write(a + 1, v & 0xff); }

/* CRTC R00-R08 and R20 */
static void mode(const WORD *r, WORD r20)
{
	int i;
	for (i = 0; i < 9; i++)
		crtc(i, r[i]);
	crtc(20, r20);
}
static const WORD m768[9] = { 0x89, 0x0e, 0x1c, 0x7c, 0x237, 0x05, 0x28, 0x228, 0x1b };
static const WORD m512[9] = { 0x5b, 0x09, 0x11, 0x51, 0x237, 0x05, 0x28, 0x228, 0x1b };
static const WORD m256[9] = { 0x25, 0x01, 0x00, 0x20, 0x237, 0x05, 0x28, 0x228, 0x1b };

static void fill_common(void)
{
	DWORD i, y, x;

	rs = 1;
	for (i = 0; i < 0x200; i += 2)		/* graphic and text palettes */
		palw(0xe82000 + i, rnd() | 1);
	for (i = 0x200; i < 0x400; i += 2)
		palw(0xe82000 + i, rnd() | 1);
	palw(0xe82000, 0);			/* colour 0 black, as programs set it */
	palw(0xe82200, 0);
	/* GVRAM: runs of 32 dots, about half of them transparent */
	for (i = 0; i < 0x80000; i += 64) {
		int on = rnd() & 1;
		for (x = 0; x < 64; x++)
			GVRAM[i + x] = on ? rnd() : 0;
	}
	/* text: some 8-dot groups set in the 4 planes */
	for (y = 0; y < 1024; y++)
		for (x = 0; x < 128; x += 2) {
			WORD v = (rnd() % 4 == 0) ? rnd() : 0;
			for (i = 0; i < 4; i++) {
				TVRAM_Write(0xe00000 + i * 0x20000 + y * 128 + x, v >> 8);
				TVRAM_Write(0xe00000 + i * 0x20000 + y * 128 + x + 1, v & 0xff);
			}
		}
	/* BG: patterns, both BG pages, 128 sprites */
	for (i = 0; i < 0x4000; i += 2)
		bgw(0xeb8000 + i, (rnd() % 3) ? rnd() : 0);
	for (i = 0; i < 0x4000; i += 2)
		bgw(0xebc000 + i, rnd() & 0x0fff);
	for (i = 0; i < 128; i++) {
		bgw(0xeb0000 + i * 8, 16 + rnd() % 512);	/* x */
		bgw(0xeb0002 + i * 8, 16 + rnd() % 512);	/* y */
		bgw(0xeb0004 + i * 8, rnd() & 0xcfff);		/* flip, colour, pattern */
		bgw(0xeb0006 + i * 8, (i < 96) ? 3 : 0);	/* priority (0: off) */
	}
}

static DWORD frame_hash;

/* draw every line of the screen once */
static double draw_frame(void)
{
	DWORD y, x, h = 2166136261u;
	for (y = 0; y < TextDotY; y++) {
		VLINE = y;
		TextDirtyLine[y] = 1;
		WinDraw_DrawLine();
	}
	for (y = 0; y < TextDotY; y += 7)
		for (x = 0; x < TextDotX; x += 3)
			h = (h ^ ScrBuf[y * 800 + x]) * 16777619u;
	frame_hash = h;
	return TextDotY;
}

static double secs = 0.3;

static void measure(const char *name)
{
	double t0, t, lines = 0;
	DWORD h;

	draw_frame();			/* warm-up */
	h = frame_hash;
	t0 = now();
	do {
		lines += draw_frame();
		t = now() - t0;
	} while (t < secs);
	printf("%s %.1f\n", name, t * 1e9 / lines);
	printf("%s_hash %08x\n", name, h);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		secs = atof(argv[1]);
	ScrBuf = calloc(800 * 1024, 2);
	CRTC_Init();
	BG_Init();
	TVRAM_Init();
	GVRAM_Init();
	Pal_Init();
	fill_common();

	/* 256x256 sprite game: BG0 + 96 sprites + text, one 16-colour graphic page */
	mode(m256, 0x0010);
	bgw(0xeb080a, 0x25); bgw(0xeb080c, 0x04); bgw(0xeb080e, 0x10); bgw(0xeb0810, 0x00);
	bgw(0xeb0808, 0x0201);		/* display on, BG0 on (page 0) */
	vctl(0xe82400, 0x0000);
	vctl(0xe82500, 0x12e4);
	vctl(0xe82600, 0x0061);		/* sprites, text, graphic page 0 */
	measure("bg256");

	/* 512x512, 16 colours, 4 graphic pages under text and sprites */
	mode(m512, 0x0015);
	bgw(0xeb080a, 0x5b); bgw(0xeb080c, 0x09); bgw(0xeb080e, 0x10); bgw(0xeb0810, 0x15);
	bgw(0xeb0808, 0x0201);
	vctl(0xe82400, 0x0000);
	vctl(0xe82500, 0x12e4);
	vctl(0xe82600, 0x006f);
	measure("g16x4");

	/* the same, graphic page 0 translucent over the others (half colour) */
	vctl(0xe82600, 0x1d6f);
	measure("g16x4_tr");

	/* 512x512, 256 colours, 2 pages, text */
	mode(m512, 0x0115);
	vctl(0xe82400, 0x0001);
	vctl(0xe82600, 0x0023);
	measure("g256x2");

	/* 512x512, 65536 colours, text */
	mode(m512, 0x0315);
	vctl(0xe82400, 0x0003);
	vctl(0xe82600, 0x002f);
	measure("g64k");

	/* 768x512 text only */
	mode(m768, 0x0016);
	vctl(0xe82400, 0x0004);
	vctl(0xe82600, 0x0020);
	measure("text768");
	return 0;
}
