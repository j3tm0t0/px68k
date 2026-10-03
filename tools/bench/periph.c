/*
 * periph.c: speed of the MFP (x68k/mfp.c) on its per-slice and polling
 * paths, with the timers set up as MFP_Init leaves them (B: /10, data 13;
 * C, D: /500, data 200, 20) and their interrupts enabled.
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

static void measure(const char *name, double (*fn)(long))
{
	long n = 1000;
	double t0, t, calls = 0;

	fn(100000);			/* warm-up */
	t0 = now();
	do {
		calls += fn(n);
		if (n < 1000000) n *= 2;
		t = now() - t0;
	} while (t < secs);
	printf("%s %.2f\n", name, t * 1e9 / calls);
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
	return 0;
}
