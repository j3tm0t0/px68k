/*
 * Host check: x68k/mfp.c MFP_Timer (per-underflow) against the original
 * per-tick loop. mfp_new_body.h must match the MFP_TIMER_RUN macro and
 * MFP_Timer body in mfp.c (MFP_IntInl -> MFP_Int).
 *   cc -O2 -o /tmp/mfp_harness tools/exacttest/mfp_harness.c && /tmp/mfp_harness
 */
/* old vs new MFP_Timer: random state, compare registers, ticks and MFP_Int log */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char BYTE;
#define MFP_TACR	12
#define MFP_TBCR	13
#define MFP_TCDCR	14
#define MFP_TADR	15
#define MFP_TBDR	16
#define MFP_TCDR	17
#define MFP_TDDR	18

static BYTE MFP[24];
static BYTE Timer_Reload[4];
static int Timer_Tick[4];
static const int Timer_Prescaler[8] = {1, 10, 25, 40, 125, 160, 250, 500};
static int logbuf[100000], logn;
static void MFP_Int(int irq) { logbuf[logn++] = irq; }

static void old_MFP_Timer(long clock)
{
	if ( (!(MFP[MFP_TACR]&8))&&(MFP[MFP_TACR]&7) ) {
		int t = Timer_Prescaler[MFP[MFP_TACR]&7];
		Timer_Tick[0] += clock;
		while ( Timer_Tick[0]>=t ) {
			Timer_Tick[0] -= t;
			MFP[MFP_TADR]--;
			if ( !MFP[MFP_TADR] ) {
				MFP[MFP_TADR] = Timer_Reload[0];
				MFP_Int(2);
			}
		}
	}
	if ( MFP[MFP_TBCR]&7 ) {
		int t = Timer_Prescaler[MFP[MFP_TBCR]&7];
		Timer_Tick[1] += clock;
		while ( Timer_Tick[1]>=t ) {
			Timer_Tick[1] -= t;
			MFP[MFP_TBDR]--;
			if ( !MFP[MFP_TBDR] ) {
				MFP[MFP_TBDR] = Timer_Reload[1];
				MFP_Int(7);
			}
		}
	}
	if ( MFP[MFP_TCDCR]&0x70 ) {
		int t = Timer_Prescaler[(MFP[MFP_TCDCR]&0x70)>>4];
		Timer_Tick[2] += clock;
		while ( Timer_Tick[2]>=t ) {
			Timer_Tick[2] -= t;
			MFP[MFP_TCDR]--;
			if ( !MFP[MFP_TCDR] ) {
				MFP[MFP_TCDR] = Timer_Reload[2];
				MFP_Int(10);
			}
		}
	}
	if ( MFP[MFP_TCDCR]&7 ) {
		int t = Timer_Prescaler[MFP[MFP_TCDCR]&7];
		Timer_Tick[3] += clock;
		while ( Timer_Tick[3]>=t ) {
			Timer_Tick[3] -= t;
			MFP[MFP_TDDR]--;
			if ( !MFP[MFP_TDDR] ) {
				MFP[MFP_TDDR] = Timer_Reload[3];
				MFP_Int(11);
			}
		}
	}
}

#define NEW_IMPL
#include "mfp_new_body.h"

int main(void)
{
	long iter;
	srand(12345);
	for (iter = 0; iter < 20000000; iter++) {
		BYTE m0[24], m1[24]; int t0[4], t1[4]; int l0[1000], n0;
		int i, k, steps = 1 + rand() % 8;
		long clocks[8];
		for (i = 0; i < 24; i++) m0[i] = rand();
		/* bias towards common/edge values */
		for (i = 15; i <= 18; i++) { int r = rand() % 6; if (r == 0) m0[i] = 0; else if (r == 1) m0[i] = 1; else if (r == 2) m0[i] = 255; }
		for (i = 0; i < 4; i++) { int r = rand() % 5; Timer_Reload[i] = r == 0 ? 0 : r == 1 ? 1 : rand(); }
		for (i = 0; i < 4; i++) t0[i] = rand() % 501 - (rand() % 10 == 0 ? 600 : 0);
		for (k = 0; k < steps; k++) {
			int r = rand() % 10;
			clocks[k] = r == 0 ? 0 : r == 1 ? rand() % 5000 : r == 2 ? -(rand() % 100) : rand() % 700;
		}
		memcpy(MFP, m0, 24); memcpy(Timer_Tick, t0, sizeof t0); logn = 0;
		for (k = 0; k < steps; k++) old_MFP_Timer(clocks[k]);
		memcpy(m1, MFP, 24); memcpy(t1, Timer_Tick, sizeof t1); n0 = logn; memcpy(l0, logbuf, n0 > 1000 ? 1000 * sizeof(int) : n0 * sizeof(int));
		{ static int big[100000]; memcpy(big, logbuf, n0 * sizeof(int));
		memcpy(MFP, m0, 24); memcpy(Timer_Tick, t0, sizeof t0); logn = 0;
		for (k = 0; k < steps; k++) new_MFP_Timer(clocks[k]);
		if (memcmp(m1, MFP, 24) || memcmp(t1, Timer_Tick, sizeof t1) || n0 != logn || memcmp(big, logbuf, n0 * sizeof(int))) {
			printf("MISMATCH iter %ld\n", iter); return 1;
		} }
	}
	printf("OK\n");
	return 0;
}
