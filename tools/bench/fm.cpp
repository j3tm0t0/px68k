// fm.cpp: speed of fmgen's OPM (YM2151) synthesis and timers, 8 channels
// playing, LFO on, timers A and B running.
//
//   fm [seconds per test]	prints "<test> <ns per sample | per call>"
#include "common.h"
#include "opm.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

class T : public FM::OPM {
public:
	void Intr(bool f) { }
};

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double secs = 0.3;
static T *opm;
static BYTE buf[4 * 4096];
static long keyed;

static void keys(void)
{
	for (int ch = 0; ch < 8; ch++) {
		opm->SetReg(0x08, ch);				// key off
		opm->SetReg(0x28 + ch, 0x30 + ((keyed + ch * 3) % 12));	// KC
		opm->SetReg(0x08, 0x78 | ch);			// key on, all slots
	}
	keyed++;
}

static void setup(void)
{
	opm = new T;
	opm->Init(4000000, 44100, true);
	opm->SetVolume(-8);
	opm->SetReg(0x18, 0xc0);		// LFO frequency
	opm->SetReg(0x19, 0x20);		// AMD
	opm->SetReg(0x19, 0x90);		// PMD
	opm->SetReg(0x1b, 0x02);		// waveform
	for (int ch = 0; ch < 8; ch++) {
		opm->SetReg(0x20 + ch, 0xc0 | (ch & 1 ? 0x24 : 0x3a));	// L/R, FB, CON 2/4
		opm->SetReg(0x30 + ch, ch << 2);			// KF
		opm->SetReg(0x38 + ch, 0x21);				// PMS, AMS
		for (int op = 0; op < 4; op++) {
			int r = op * 8 + ch;
			opm->SetReg(0x40 + r, 0x01 + op);		// DT1, MUL
			opm->SetReg(0x60 + r, op == 3 ? 0x08 : 0x20);	// TL
			opm->SetReg(0x80 + r, 0x1f);			// KS, AR
			opm->SetReg(0xa0 + r, 0x85);			// AMS-EN, D1R
			opm->SetReg(0xc0 + r, 0x02);			// DT2, D2R
			opm->SetReg(0xe0 + r, 0x2f);			// D1L, RR
		}
	}
	opm->SetReg(0x10, 0xc8);		// timer A
	opm->SetReg(0x11, 0x00);
	opm->SetReg(0x12, 0xe0);		// timer B
	opm->SetReg(0x14, 0x3f);		// load, enable, reset A and B
	keys();
}

// samples per Mix call: 1 is what WinX68k_Exec's per-line DSound_Send0 does
static int chunk;
static double run_mix(long n)
{
	BYTE *pbsp = buf, *pbep = buf + sizeof buf;
	long done = 0;
	unsigned pos = 0;
	while (done < n) {
		opm->Mix((FM::Sample *)(buf + pos), chunk, 0, pbsp, pbep);
		pos = (pos + chunk * 4) % sizeof buf;
		done += chunk;
		if (done % 4410 < (unsigned)chunk)
			keys();
	}
	return done;
}

// Count() per raster line (~32 us), as OPM_Timer does
static double run_count(long n)
{
	for (long i = 0; i < n; i++) {
		opm->Count(32);
		if ((i & 255) == 0)
			opm->SetReg(0x14, 0x3f);	// as an interrupt handler would: reset the flags
	}
	return n;
}

static void measure(const char *name, double (*fn)(long))
{
	long n;
	double t0, t, calls, best = 0;
	int w;

	fn(100000);			// warm-up
	for (w = 0; w < 3; w++) {	// the best of 3 windows: interference only slows down
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
	setup();
	chunk = 1;
	measure("opm_mix1", run_mix);
	chunk = 64;
	measure("opm_mix64", run_mix);
	measure("opm_count", run_count);
	return 0;
}
