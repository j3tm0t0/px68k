// Used by fmcompare.sh.
// Drives fmgen OPM with random register writes / timer counts / mixes; prints a hash.
#include "common.h"
#include "opm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long h = 1469598103934665603ULL;
static void H(unsigned v) { h = (h ^ v) * 1099511628211ULL; }

class T : public FM::OPM {
public:
	void Intr(bool f) { H(0x1000 + f); }
};

static unsigned rs = 2463534242u;
static unsigned R() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static BYTE buf[4 * 4 * 4096];

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 2000000;
	T *opm = new T;
	opm->Init(4000000, 11025, true);
	opm->SetVolume(-8);
	for (unsigned i = 0; i < sizeof buf; i++) buf[i] = R();
	for (long it = 0; it < n; it++) {
		unsigned a = R() % 100;
		if (a < 45) {
			unsigned reg, d = R() & 0xff;
			unsigned k = R() % 10;
			if (k < 3) reg = 0x08;			/* key on/off */
			else if (k < 4) reg = 0x10 + R() % 5;	/* timers / CSM */
			else if (k < 5) reg = R() % 0x40;
			else reg = 0x40 + R() % 0xc0;
			if (reg == 0x08 && (R() & 1)) d = (d & 7) | 0x78;
			if (reg >= 0x60 && reg < 0x80 && (R() & 1)) d &= 0x1f;	/* audible TL */
			opm->SetReg(reg, d);
		} else if (a < 70) {
			int us = R() % 300;
			H(opm->Count(us));
			H(opm->ReadStatus());
			H(opm->GetNextEvent());
		} else {
			static const int rates[4] = { 11025, 22050, 44100, 0 };
			int rate = rates[R() % 4];
			int len = 1 + R() % 64;
			unsigned start = (R() % 4096) * 4;
			BYTE *pbsp = buf, *pbep = buf + sizeof buf;
			opm->Mix((FM::Sample *)(buf + start), len, rate, pbsp, pbep);
			int dup = rate == 11025 ? 4 : rate == 22050 ? 2 : 1;
			for (unsigned i = 0, p = start; i < (unsigned)(len * dup); i++, p = (p + 4) % sizeof buf)
				H(*(unsigned *)(buf + p));
		}
	}
	for (unsigned i = 0; i < sizeof buf; i += 4) H(*(unsigned *)(buf + i));
	printf("%016llx\n", h);
	return 0;
}
