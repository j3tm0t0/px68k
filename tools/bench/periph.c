/*
 * periph.c: speed of the per-slice and per-line peripheral work of
 * WinX68k_Exec: the MFP (x68k/mfp.c) with the timers set up as MFP_Init
 * leaves them (B: /10, data 13; C, D: /500, data 200, 20) and their
 * interrupts enabled, its GPIP reads, RTC_Timer (x68k/rtc.c) per slice and
 * ADPCM_PreUpdate (x68k/adpcm.c, idle) per line.  clknext_*: the two ways
 * WinX68k_Exec works out the next line boundary (copies of its code: it
 * cannot be called alone).
 *
 *   periph [seconds per test]	prints "<test> <ns per call>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "winx68k.h"
#include "crtc.h"
#include "mfp.h"
#include "rtc.h"
#include "adpcm.h"
#include "dmac.h"
#include "prop.h"

/* what mfp.c uses from the rest of the emulator */
int ICount;
DWORD vline;
WORD VLINE_TOTAL = 568, CRTC_VSTART = 40, CRTC_VEND = 552, CRTC_IntLine = 1023;
int HSYNC_CLK = 324;
BYTE CRTC_Regs[48];
void Error(const char *s) { }
BYTE traceflag;
BYTE KeyBufRP, KeyBufWP, KeyIntFlag;
static unsigned nint;
void IRQH_Int(BYTE irq, void *handler) { nint++; }
void IRQH_IRQCallBack(BYTE irq) { }
Win68Conf Config;
dmac_ch DMA[4];
int FASTCALL DMA_Exec(int ch) { return 0; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double secs = 0.3;
static volatile unsigned sink;

/* MFP_Timer after every 200-clock slice, as WinX68k_Exec does */
static double run_timer(long n)
{
	long i;
	for (i = 0; i < n; i++)
		MFP_Timer(200);
	return n;
}

/*
 * GPIP reads: reads per slice (1 with the idle loop skip of C68K, ~10 for a
 * 20-cycle btst loop without it); ICount moves on between slices
 */
static int reads_per_slice;
static double run_gpip(long n)
{
	long i;
	int k;
	for (i = 0; i < n; i++) {
		ICount -= 200;
		if (ICount < 0) {
			ICount += 180000;
			vline = (vline + 1) % 568;
		}
		for (k = 0; k < reads_per_slice; k++)
			sink += MFP_Read(0xe88001);
	}
	return (double)n * reads_per_slice;
}

/* RTC_Timer after every slice */
static double run_rtc(long n)
{
	long i;
	for (i = 0; i < n; i++)
		RTC_Timer(200);
	return n;
}

/* ADPCM_PreUpdate every raster line (clk_line ~316 at 31 kHz) */
static double run_adpcm(long n)
{
	long i;
	for (i = 0; i < n; i++)
		ADPCM_PreUpdate(316);
	return n;
}

/*
 * clk_next per raster line, as WinX68k_Exec before and after a76ec99: a
 * division per line, or quotient/remainder steps (with the redo when
 * VLINE_TOTAL changes or the numerator wraps).
 */
static volatile int clk_total_v = 180000, vt_v = 568;
static double run_clknext_div(long n)
{
	long i;
	int clk_total = clk_total_v, vlt = vt_v, vline = 0, clk_next;
	for (i = 0; i < n; i++) {
		vline++;
		clk_next = (clk_total*(vline+1))/vlt;
		sink += clk_next;
		if (vline >= vlt) vline = 0;
	}
	return n;
}
static double run_clknext_step(long n)
{
	long i;
	int clk_total = clk_total_v, vline = 0;
	DWORD cn_num = clk_total, cn_vt = vt_v, cn_q = cn_num/cn_vt, cn_r = cn_num%cn_vt, cn_sq = cn_q, cn_sr = cn_r;
	for (i = 0; i < n; i++) {
		vline++;
		cn_num += (DWORD)clk_total;
		if ( (cn_vt==(DWORD)vt_v)&&(cn_num>=(DWORD)clk_total) ) {
			cn_q += cn_sq;
			cn_r += cn_sr;
			if ( cn_r>=cn_vt ) {
				cn_r -= cn_vt;
				cn_q++;
			}
		} else {
			cn_vt = (DWORD)vt_v;
			cn_q  = cn_num/cn_vt;
			cn_r  = cn_num%cn_vt;
			cn_sq = (DWORD)clk_total/cn_vt;
			cn_sr = (DWORD)clk_total%cn_vt;
		}
		sink += cn_q;
		if (vline >= (int)cn_vt) { vline = 0; cn_num = clk_total; cn_q = cn_sq; cn_r = cn_sr; }
	}
	return n;
}

static void measure(const char *name, double (*fn)(long))
{
	long n;
	double t0, t, calls, best = 0;
	int w;

	fn(100000);			/* warm-up */
	for (w = 0; w < 3; w++) {	/* the best of 3 windows: interference only slows down */
		n = 1000;
		calls = 0;
		t0 = now();
		do {
			calls += fn(n);
			if (n < 1000000) n *= 2;
			t = now() - t0;
		} while (t < secs / 3);
		if (!w || t * 1e9 / calls < best)
			best = t * 1e9 / calls;
	}
	printf("%s %.3f\n", name, best);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		secs = atof(argv[1]);
	CRTC_Regs[1] = 0x0e;		/* 768x512 31 kHz horizontal timing */
	CRTC_Regs[5] = 0x1c;
	CRTC_Regs[7] = 0x7c;
	MFP_Init();
	MFP[MFP_IERA] = MFP[MFP_IERB] = 0xff;
	MFP[MFP_IMRA] = MFP[MFP_IMRB] = 0xff;
	ICount = 180000;
	measure("mfp_timer", run_timer);
	reads_per_slice = 1;
	measure("gpip_read1", run_gpip);
	reads_per_slice = 10;
	measure("gpip_read10", run_gpip);
	RTC_Init();
	measure("rtc_timer", run_rtc);
	ADPCM_Init(44100);
	measure("adpcm_pre", run_adpcm);
	measure("clknext_div", run_clknext_div);
	measure("clknext_step", run_clknext_step);
	return 0;
}
