/*
 * For rend.c variants with a gvram.c that has no Grp_DrawLine4Multi: the
 * calls WinDraw_DrawLine made before it, one Grp_DrawLine4 per page in the
 * same order, the first one opaque.
 */
#include "common.h"
#include "gvram.h"

void FASTCALL Grp_DrawLine4Multi(DWORD pages, int n)
{
	int k;

	for (k = 0; k < n; k++)
		Grp_DrawLine4((pages >> (k * 2)) & 3, k == 0);
}
