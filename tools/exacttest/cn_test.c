/*
 * Host check: the quotient/remainder stepping of clk_next in WinX68k_Exec
 * against (clk_total*(vline+1))/VLINE_TOTAL, with VLINE_TOTAL changes and
 * 32-bit wrap.   cc -O2 -o /tmp/cn_test tools/exacttest/cn_test.c && /tmp/cn_test
 */
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int DWORD; typedef unsigned short WORD;
static unsigned rs = 88172645u;
static unsigned R(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
int main(void)
{
	long f;
	for (f = 0; f < 200000; f++) {
		int clk_total = (R() & 1) ? 180000 + R() % 300000 : 1 + R() % 0x7fffffff;
		WORD VLINE_TOTAL = 1 + R() % ((R() & 1) ? 1100 : 65535);
		DWORD vline = 0;
		int clk_next, ref;
		DWORD cn_num, cn_vt, cn_q, cn_r, cn_sq, cn_sr;
		clk_next = (clk_total/VLINE_TOTAL);
		cn_num = (DWORD)clk_total; cn_vt = VLINE_TOTAL; cn_q = cn_num/cn_vt; cn_r = cn_num%cn_vt; cn_sq = cn_q; cn_sr = cn_r;
		if ((int)cn_q != clk_next) { printf("init mismatch\n"); return 1; }
		while (vline < 3000) {
			if (R() % 500 == 0) VLINE_TOTAL = 1 + R() % 65535;
			vline++;
			ref = (clk_total*(vline+1))/VLINE_TOTAL;
			cn_num += (DWORD)clk_total;
			if ( (cn_vt==(DWORD)VLINE_TOTAL)&&(cn_num>=(DWORD)clk_total) ) {
				cn_q += cn_sq; cn_r += cn_sr;
				if ( cn_r>=cn_vt ) { cn_r -= cn_vt; cn_q++; }
			} else {
				cn_vt = (DWORD)VLINE_TOTAL;
				cn_q = cn_num/cn_vt; cn_r = cn_num%cn_vt;
				cn_sq = (DWORD)clk_total/cn_vt; cn_sr = (DWORD)clk_total%cn_vt;
			}
			clk_next = (int)cn_q;
			if (clk_next != ref) { printf("mismatch f=%ld vline=%u ct=%d vt=%u %d %d\n", f, vline, clk_total, VLINE_TOTAL, clk_next, ref); return 1; }
		}
	}
	printf("OK\n");
	return 0;
}
