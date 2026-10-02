/*
 * GE compositing of the X68000 screen (PSP), see gecomp.h.
 *
 * Supported (anything else is left to the CPU path, line by line):
 *   16 colour 512 dot graphics (any pages, any page order), no translucency
 *   or special priority (VCReg2 bit 12 clear), text, 8x8/16x16 BG, sprites,
 *   any text/BG/graphics priority for which the CPU result is "graphics
 *   below text and BG" or "no graphics", up to 512 dots wide, lines 0-255.
 *
 * What the CPU path (x11/windraw.c DrawLine) does for these modes and how it
 * is reproduced:
 *
 *  - Text and BG/sprites are merged into one line, BG_LineBuf (here: the
 *    "layer", drawn into a 512 x 256 RGB565 buffer in VRAM):
 *      BG above text: text, opaque (TextPal[t] for every dot), then BG and
 *        sprites over it where their dot is not 0 (or TextPal[0] first if the
 *        text is off);
 *      text above BG: TextPal[0] (0 if text and BG are off), BG and sprites
 *        where their dot is not 0 - and, the "gd" rule, a BG tile with palette
 *        block != 0 also draws TextPal[block * 16] where its dot is 0 and no
 *        sprite/BG dot was drawn before: here drawn as a rectangle of that
 *        colour under everything - then text where its dot is not 0.
 *    BG/sprite drawing order: sprites of priority 1, BG1 (8x8 only), sprites
 *    of priority 2, BG0, sprites of priority 3.  A sprite dot is only drawn
 *    where no sprite with a lower number drew a dot before on this line
 *    (BG_PriBuf, not reset between the priorities, not touched by BG dots):
 *    the depth buffer holds BG_PriBuf, the sprites are drawn with depth n and
 *    the test "less or equal".
 *  - The graphic pages: the first is opaque (GrphPal[dot]), the others are
 *    drawn where their dot is not 0, all into the screen texture directly.
 *  - Graphics, then the layer where its colour is not 0 (colour test), or
 *    the layer alone (copied), or zeros.  The text/BG flags (Text_TrFlag) only
 *    matter when the graphics are between text and BG: not supported.
 *
 * Exact colours: the palettes are RGB565 values; the CLUTs hold them as 8888
 * (5 -> 8 bits by replication, alpha 0 for transparent dots) and the GE
 * writes them back to 565 by truncation: the same 16-bit values.  No
 * dithering, no filtering, no blending.
 */

#include <stddef.h>
#include <string.h>
#include <pspkernel.h>
#include <pspgu.h>

#include "common.h"
#include "winx68k.h"
#include "crtc.h"
#include "bg.h"
#include "palette.h"
#include "gvram.h"
#include "tvram.h"
#include "gecomp.h"
#include "log.h"

extern BYTE Debug_Text, Debug_Grp, Debug_Sp;
extern BYTE Sprite_Regs[0x800];
extern BYTE BG[0x8000];

int GE_Enabled = 1;	/* GE compositing where supported; "ge off" for the CPU path */
int GE_TimeSync = 0;
volatile int GE_Guard = 0;
int GE_PalDirty = 1;
unsigned GE_StatLines, GE_StatBands, GE_StatFlushes, GE_StatCpuLines;

/* VRAM (offsets from 0x04000000); see the map in windraw.c */
#define GE_SCRBUF_L	0x000cc000	/* screen texture, 512 x 512 (windraw.c ScrBufL) */
#define GE_LAYER	0x0014c000	/* text/BG layer, 512 x 256: ScrBufR, unused up to 512 dots */
#define GE_ATLAS16	0x001d0000	/* 256 16x16 patterns, T4, 512 x 128 */
#define GE_ATLAS8	0x001d8000	/* 256 8x8 patterns, T4, 512 x 32 */
#define GE_VRAM(off)	((void *)(0x04000000 | (off)))
#define GE_UNCACHED(off) ((BYTE *)(0x44000000 | (off)))
#define GE_ROWS		256		/* lines the layer and the depth buffer have */

/* ---- BG pattern shadows ----
 * T4 textures take the left dot from the low nibble, BG[] has it in the high
 * nibble: nibble-swapped copies, written uncached by the CPU. */

void GE_BGWrite(DWORD adr, BYTE data)
{
	const BYTE d = (BYTE)((data >> 4) | (data << 4));
	DWORD p = adr >> 7;

	/* 16x16: pattern p at (p & 31) * 16, (p >> 5) * 16; BG[] holds the
	 * left 8 dots of rows 0-15, then the right 8 dots */
	GE_UNCACHED(GE_ATLAS16)[((p >> 5) * 16 + ((adr >> 2) & 15)) * 256
				+ (p & 31) * 8 + ((adr >> 4) & 4) + (adr & 3)] = d;
	if (adr < 0x2000) {
		/* 8x8: pattern p at (p & 63) * 8, (p >> 6) * 8 */
		p = adr >> 5;
		GE_UNCACHED(GE_ATLAS8)[((p >> 6) * 8 + ((adr >> 2) & 7)) * 256
				       + (p & 63) * 4 + (adr & 3)] = d;
	}
}

void GE_BGReset(void)
{
	DWORD a;

	memset(GE_UNCACHED(GE_ATLAS16), 0, 256 * 128);
	memset(GE_UNCACHED(GE_ATLAS8), 0, 256 * 32);
	for (a = 0; a < 0x8000; a++)
		if (BG[a])
			GE_BGWrite(a, BG[a]);
}

/* ---- per line state ---- */

#define GE_LIVE	0xff	/* GE_State.spr: Sprite_Regs itself */

enum { GE_ZERO, GE_G, GE_M, GE_GM };

typedef struct {
	BYTE	mode;		/* GE_ZERO: zeros, GE_G: graphics, GE_M: layer, GE_GM: both */
	BYTE	ng;		/* graphic pages drawn; the first is opaque */
	BYTE	gpage[4];
	BYTE	mcase;		/* layer: 0 BG above text, 1 text above BG */
	BYTE	ton;		/* layer: text on */
	BYTE	bgon;		/* layer: BG/sprites on */
	BYTE	chr8;		/* 8x8 BG */
	BYTE	bg9;		/* BG_Regs[9] */
	BYTE	pal;		/* palette snapshot */
	BYTE	spr;		/* sprite register snapshot, GE_LIVE: Sprite_Regs */
	BYTE	gm;		/* graphics: 0 16 colours (gpage: 4-bit pages), 1 256 colours (bytes), 2 65536 */
	BYTE	prio;		/* GE_GM, graphics between text and BG: see ge_prio */
	BYTE	pbefore;	/* ... the layer is drawn (opaque) before the graphics */
	BYTE	pafter;		/* ... and after them where: 1 text dot, 2 BG/sprite dot (flags), 4 not 0 */
	WORD	dotx;
	WORD	gx[4], gy[4];	/* graphic scroll, & 511 */
	WORD	tx, ty;		/* text scroll, & 1023 */
	WORD	bg0top, bg1top;
	DWORD	bg0sx, bg0sy, bg1sx, bg1sy;
	DWORD	hadj;		/* BG_HAdjust */
	DWORD	lbase;		/* VLINEBG - BG_VLINE - VLINE */
} GE_State;

typedef struct {
	GE_State st;
	WORD	y0, h;
	short	rec0, rec1;	/* ge_rec[] of its BG/sprite draws (ge_prio), */
	unsigned recbuild;	/* ... recorded in this build (ge_buildno), else 0 */
} GE_Band;

#define GE_NBAND	32	/* worst case (8x8 BG, 512 dots, 1-line bands) ~500 KB of the 1 MB display list */
#define GE_NPAL		8

static GE_Band ge_band[GE_NBAND];
static int ge_nband;

typedef struct {
	WORD	text[256];
	WORD	grp[256];
	BYTE	regs[512];	/* the graphic palette registers (65536 colours) */
} GE_Pal;

static GE_Pal ge_pal[GE_NPAL];
static int ge_npal, ge_palcur;

/* 0, or why the CPU must draw this line (GE_R_*) */
static int ge_eval(GE_State *s)
{
	const int v1 = VCReg1[0], v11 = VCReg1[1], v21 = VCReg2[1];
	int gp, tp, sp, text_on, bg_on, ton, bgon, gon, tdrawed = 0;
	int nops = 0, mseen = 0, gpos = -1, before = 0, after = 0;

	memset(s, 0, sizeof(*s));
	s->spr = GE_LIVE;
	if (!Debug_Grp || !Debug_Text || !Debug_Sp)
		return GE_R_DEBUG;
	if (VCReg0[1] & 4)
		return GE_R_GMODE;	/* 16, 256 or 65536 colours 512 dots */
	if (VCReg2[0] & 0x10)	/* no translucency / special priority */
		return GE_R_TRANS;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		return GE_R_R29;
	if (TextDotX == 0 || TextDotX > 512)
		return GE_R_WIDTH;
	s->dotx = TextDotX;

	if (!(VCReg0[1] & 3)) {
		/* graphic pages, as in DrawLine (Grp_DrawLine4) */
		if (v21 & 8)
			s->gpage[s->ng++] = (v11 >> 6) & 3;
		if (v21 & 4)
			s->gpage[s->ng++] = (v11 >> 4) & 3;
		if (v21 & 2)
			s->gpage[s->ng++] = (v11 >> 2) & 3;
		if (v21 & 1)
			s->gpage[s->ng++] = v11 & 3;
	} else if ((VCReg0[1] & 3) == 3) {
		/* 65536 colours (Grp_DrawLine16): the words, page 0's scroll */
		s->gm = 2;
		if (v21 & 15)
			s->gpage[s->ng++] = 0;
	} else {
		/*
		 * 256 colours, as in DrawLine (Grp_DrawLine8): page p is byte p of
		 * the GVRAM words, its low nibble scrolled as 16 colour page
		 * 2p, its high one as 2p + 1 (only drawn here when both scroll
		 * alike).  Bit 2 of VC R2 draws the page of lower priority first
		 * (GRP0's when equal: "ドラスピ"), bit 0 the other.
		 */
		const int first = ((v11 & 3) <= ((v11 >> 4) & 3)) ? 1 : 0;

		s->gm = 1;
		if (v21 & 4)
			s->gpage[s->ng++] = first;
		if (v21 & 1)
			s->gpage[s->ng++] = first ^ 1;
	}
	gon = s->ng != 0;

	gp = v1 & 3;
	tp = (v1 >> 2) & 3;
	sp = (v1 >> 4) & 3;
	text_on = (v21 & 0x20) != 0;
	bg_on = (v21 & 0x40) && (BG_Regs[8] & 2) && !(BG_Regs[0x11] & 2);
	s->mcase = !(sp < tp);
	ton = text_on;
	bgon = s->mcase ? 1 : bg_on;	/* text above BG: BG_LineBuf is always filled */

	/*
	 * The drawing steps of DrawLine; G: graphics (once), M: BG_LineBuf (the
	 * layer), by its BG step (k 2) or its text step (k 1), all of it or
	 * (m) only where Text_TrFlag & k.  All M steps draw the same layer: the
	 * first step is opaque, the others draw where the layer is not 0.  So
	 * the line is: before the graphics, the layer (if it came first), then
	 * the graphics where not 0, then the layer where the steps after the
	 * graphics draw it ("after": 4 all of it, else the flags).
	 */
#define OPG()	do { gpos = nops++; } while (0)
#define OPM(m, k)	do { if (gpos < 0) before |= !nops; else after |= (m) ? (k) : 4; \
			     mseen = 1; nops++; } while (0)
	if ((gp & 2) && gon)
		OPG();
	if ((sp & 2) && bgon) {
		OPM(tdrawed, 2);
		tdrawed = 1;
	}
	if ((tp & 2) && ton) {
		OPM(tdrawed, 1);
		tdrawed = 1;
	}
	if (gp == 1 && gon)
		OPG();
	if (sp == 1 && bgon) {
		OPM(tp == 2, 2);
		tdrawed = 1;
	}
	if (tp == 1 && ton) {
		OPM(sp >= 1, 1);
		tdrawed = 1;
	}
	if (gp == 0 && gon)
		OPG();
	if (sp == 0 && bgon)
		OPM(tp >= 1, 2);
	if (tp == 0 && ton)
		OPM(1, 1);
#undef OPG
#undef OPM

	if (!nops)
		s->mode = GE_ZERO;
	else if (gpos < 0)
		s->mode = GE_M;
	else if (!mseen)
		s->mode = GE_G;
	else {
		s->mode = GE_GM;
		if (before || !(after & 4)) {
			/* not "graphics, then the layer where not 0": ge_prio */
			s->prio = 1;
			s->pbefore = before;
			s->pafter = (after & 4) ? 4 : after;
		}
	}

	if (s->mode == GE_G || s->mode == GE_GM) {
		int p;

		for (p = 0; p < 4; p++) {
			s->gx[p] = GrphScrollX[p] & 0x1ff;
			s->gy[p] = GrphScrollY[p] & 0x1ff;
		}
		if (s->gm == 2) {
			s->gx[1] = s->gx[2] = s->gx[3] = 0;
			s->gy[1] = s->gy[2] = s->gy[3] = 0;
		} else if (s->gm) {
			/* byte page p: the scroll of its nibble pages 2p, 2p + 1 */
			int k;

			for (k = 0; k < s->ng; k++) {
				p = s->gpage[k];
				if (s->gx[p * 2] != s->gx[p * 2 + 1] || s->gy[p * 2] != s->gy[p * 2 + 1])
					return GE_R_GMODE;
			}
			s->gx[1] = s->gx[2];
			s->gy[1] = s->gy[2];
			s->gx[2] = s->gx[3] = s->gy[2] = s->gy[3] = 0;
		}
	} else {
		s->ng = 0;
		memset(s->gpage, 0, sizeof(s->gpage));
	}

	if (s->mode == GE_M || s->mode == GE_GM) {
		s->ton = text_on;
		s->bgon = bg_on;
		if (text_on) {
			s->tx = TextScrollX & 0x3ff;
			s->ty = TextScrollY & 0x3ff;
			if (s->tx + s->dotx > 1024)
				return GE_R_TWRAP;	/* the line wraps: odd last dot in Text_DrawLine */
		}
		if (bg_on) {
			const int s1 = ((BG_Regs[0x11] & 4) ? 2 : 1) - ((BG_Regs[0x11] & 16) ? 1 : 0);
			const int s2 = ((CRTC_Regs[0x29] & 4) ? 2 : 1) - ((CRTC_Regs[0x29] & 16) ? 1 : 0);
			DWORD vbg = VLINE;

			if (s1 != s2)
				return GE_R_BGRES;	/* VLINEBG not VLINE + constant */
			if (!(BG_Regs[0x11] & 16))
				vbg -= ((BG_Regs[0x0f] >> s1) - (CRTC_Regs[0x0d] >> s2));
			s->lbase = vbg - (DWORD)BG_VLINE - VLINE;
			s->chr8 = BG_CHRSIZE == 8;
			s->bg9 = BG_Regs[9];
			s->bg0top = BG_BG0TOP;
			s->bg1top = BG_BG1TOP;
			s->bg0sx = BG0ScrollX;
			s->bg0sy = BG0ScrollY;
			s->bg1sx = BG1ScrollX;
			s->bg1sy = BG1ScrollY;
			s->hadj = (DWORD)BG_HAdjust;
		}
	} else
		s->mcase = 0;
	return 0;
}

int GE_Pending(void)
{
	return ge_nband != 0;
}

/* ---- what the waiting/drawn bands read: the guards ----
 *
 * Writes go ahead unless they hit memory that a band waiting to be drawn
 * (or being drawn by the GE) reads:
 *   GVRAM: per page, the rows and the columns shown (and the odd dot);
 *   text: the TextDrawWork rows and 8-dot columns shown;
 *   BG[]: the map entries and patterns (16x16 and 8x8) used, worked out
 *     when a BG write comes; the pattern copies the GE reads are only
 *     updated when the GE is done (queued meanwhile);
 *   sprite registers: copied for the waiting bands at the first write.
 * Anything else (fast clear, raster copy, other GVRAM layouts) waits.
 *
 * The masks of the waiting bands and of those handed to the GE are kept
 * apart: a write that only hits the latter waits for the GE (if it is not
 * done already) but does not draw the waiting bands, and the GE may still
 * draw the last frame while the lines of this one are recorded.
 */

static int ge_inflight;		/* bands handed to the GE, not done yet */
static int ge_g16pend, ge_g16pal;	/* waiting 65536 colour bands, their palette */
static int ge_g16fly;		/* ... and some of those handed to the GE */
static int ge_voff, ge_coff;	/* list memory used (see ge_mem) */
static int ge_vneed;		/* ge_varena bytes the waiting bands may take */
static int ge_bgfly;		/* ... and some of them read the BG patterns */
typedef struct {
	DWORD	grow[4][16], gcol[4][16];	/* GVRAM rows/columns read, per page */
	DWORD	trow[32], tcol[4];		/* TextDrawWork rows, 8-dot columns read */
	int	any;				/* not all 0 */
} GE_Masks;
static GE_Masks ge_mk[2];	/* 0: the waiting bands, 1: the bands handed to the GE */
static int ge_bgpend;		/* waiting bands with BG/sprites */

#define GE_BIT(m, i)	((m)[(i) >> 5] & (1u << ((i) & 31)))
#define GE_SET(m, i)	((m)[(i) >> 5] |= 1u << ((i) & 31))

/* BG use of the waiting bands, made on demand */
static int ge_usevalid;
static DWORD ge_use16[8], ge_use8[8];	/* patterns */
static DWORD ge_usemap[0x4000 / 32];	/* BG[] words (map entries) */

#define GE_NSPR		16
static BYTE ge_spr[GE_NSPR][0x400] __attribute__((aligned(4)));
static int ge_nspr;

#define GE_NQ		4096
static WORD ge_qadr[GE_NQ];
static BYTE ge_qdat[GE_NQ];
static int ge_nq;

unsigned GE_Stat[GE_ST_N];

/* the waiting bands were handed to the GE */
static void ge_handed(void)
{
	GE_Masks *const w = &ge_mk[0], *const f = &ge_mk[1];

	if (w->any) {
		DWORD *d = &f->grow[0][0];
		const DWORD *s = &w->grow[0][0];
		int i;

		for (i = 0; i < (int)(offsetof(GE_Masks, any) / sizeof(DWORD)); i++)
			d[i] |= s[i];
		f->any = 1;
		memset(w, 0, sizeof(*w));
	}
	ge_bgpend = 0;
	ge_usevalid = 0;
	ge_nspr = 0;
	if (ge_g16pend)
		ge_g16fly = 1;
	ge_g16pend = 0;
}

/* the GE is done with what it was given (windraw.c, after sceGuSync) */
void GE_Done(void)
{
	int i;

	ge_inflight = 0;
	ge_bgfly = 0;
	ge_g16fly = 0;
	ge_voff = ge_coff = 0;	/* the GE is idle */
	if (ge_mk[1].any)
		memset(&ge_mk[1], 0, sizeof(ge_mk[1]));
	if (ge_nq) {
		const unsigned t0 = sceKernelGetSystemTimeLow();

		for (i = 0; i < ge_nq; i++)
			GE_BGWrite(ge_qadr[i], ge_qdat[i]);
		ge_nq = 0;
		GE_Stat[GE_ST_DONE_US] += sceKernelGetSystemTimeLow() - t0;
	}
	GE_Guard = ge_nband != 0;
}

/* draw the waiting bands and wait until the GE is done */
static void ge_sync(int why)
{
	GE_Stat[why]++;
	WinDraw_GESync();
}

/* wait until the GE is done (with the bands handed to it) */
static void ge_wait(int why)
{
	GE_Stat[why]++;
	WinDraw_GEWait();
}

/* the masks for line VLINE of band b (new: the band starts here) */
static void ge_mark_line(const GE_Band *b, int new)
{
	const GE_State *s = &b->st;
	GE_Masks *const m = &ge_mk[0];
	int k, i;

	if (s->mode == GE_G || s->mode == GE_GM) {
		for (k = 0; k < s->ng; k++) {
			const int p = s->gpage[k];
			const int row = (s->gy[p] + VLINE) & 511;
			const int quirk = !s->gm && 511 - s->gx[p] < s->dotx;
			/*
			 * The masks are per 16 colour page: a 256 colour page is
			 * two, the 65536 colour one all four.
			 */
			const int m0 = s->gm == 2 ? 0 : s->gm ? p * 2 : p;
			const int m1 = s->gm == 2 ? 3 : s->gm ? p * 2 + 1 : p;
			int q;

			for (q = m0; q <= m1; q++)
				GE_SET(m->grow[q], row);
			if (quirk)
				GE_SET(m->grow[p], (row - 1) & 511);
			if (new) {
				for (q = m0; q <= m1; q++)
					for (i = 0; i < s->dotx; i++)
						GE_SET(m->gcol[q], (s->gx[p] + i) & 511);
				if (!s->gm)
					GE_SET(m->gcol[p], 511);
			}
		}
		m->any = 1;
	}
	if ((s->mode == GE_M || s->mode == GE_GM) && s->ton) {
		GE_SET(m->trow, (s->ty + VLINE) & 1023);
		if (new)
			for (i = s->tx >> 3; i <= (s->tx + s->dotx - 1) >> 3; i++)
				GE_SET(m->tcol, i);
		m->any = 1;
	}
	if ((s->mode == GE_M || s->mode == GE_GM) && s->bgon) {
		ge_bgpend = 1;
		ge_usevalid = 0;
	}
}

void GE_GvramGuard(DWORD adr)
{
	DWORD a = (adr ^ 1) - 0xc00000;
	const int r28 = CRTC_Regs[0x28];
	int k, p0, p1;

	if (!ge_mk[0].any && !ge_mk[1].any)
		return;
	if ((r28 & 8) || (r28 & 7) == 3) {
		/* 65536 colours (GVRAM_Write): GVRAM[a], the word of all 16 colour pages */
		if (a >= 0x80000)
			return;
		p0 = 0;
		p1 = 3;
	} else if (r28 & 4) {
		ge_sync(GE_ST_GVRAM_MODE);	/* 1024 dots */
		return;
	} else if (a & 1)
		return;		/* not written in this mode */
	else if (r28 & 3) {
		/* 256 colours (GVRAM_Write): byte p of the word, 16 colour pages 2p, 2p + 1 */
		if (a >= 0x100000)
			return;
		if (a & 0x80000)
			a++;
		a &= 0x7ffff;
		p0 = (a & 1) * 2;
		p1 = p0 + 1;
	} else
		p0 = p1 = (a >> 19) & 3;
	for (k = 0; k < 2; k++) {
		const GE_Masks *const m = &ge_mk[k];
		const int row = (a >> 10) & 511, col = (a >> 1) & 511;
		int q, hit = 0;

		if (m->any)
			for (q = p0; q <= p1; q++)
				if (GE_BIT(m->grow[q], row) && GE_BIT(m->gcol[q], col))
					hit = 1;
		if (hit) {
			if (k == 0)
				ge_sync(GE_ST_GVRAM);
			else
				ge_wait(GE_ST_GVRAM);
			return;
		}
	}
}

void GE_TvramGuard(DWORD adr)
{
	int k;

	for (k = 0; k < 2; k++) {
		const GE_Masks *const m = &ge_mk[k];

		if (m->any && GE_BIT(m->trow, (adr >> 7) & 0x3ff) && GE_BIT(m->tcol, adr & 0x7f)) {
			if (k == 0)
				ge_sync(GE_ST_TVRAM);
			else
				ge_wait(GE_ST_TVRAM);
			return;
		}
	}
}

void GE_FullGuard(int why)
{
	ge_sync(why);
}

/* sprite registers: the waiting bands that read them get a copy */
void GE_SpriteGuard(void)
{
	int i, need = 0;

	for (i = 0; i < ge_nband; i++)
		if (ge_band[i].st.bgon && ge_band[i].st.spr == GE_LIVE)
			need = 1;
	if (!need)
		return;
	if (ge_nspr == GE_NSPR) {
		ge_sync(GE_ST_SPR_FULL);
		return;
	}
	memcpy(ge_spr[ge_nspr], Sprite_Regs, 0x400);
	for (i = 0; i < ge_nband; i++)
		if (ge_band[i].st.bgon && ge_band[i].st.spr == GE_LIVE)
			ge_band[i].st.spr = ge_nspr;
	ge_nspr++;
	GE_Stat[GE_ST_SPR_COPY]++;
}

static void ge_mark_bg(void);

/* BG[] is about to be written: maps/patterns of the waiting bands, pattern copies */
void GE_BGData(DWORD adr, BYTE data)
{
	if (ge_nband && ge_bgpend) {
		if (!ge_usevalid)
			ge_mark_bg();
		if (GE_BIT(ge_usemap, adr >> 1) || GE_BIT(ge_use16, adr >> 7) ||
		    (adr < 0x2000 && GE_BIT(ge_use8, adr >> 5))) {
			/*
			 * The CPU reads the maps when it builds the list, the GE
			 * the pattern copies, which change once it is done: the
			 * bands are handed to the GE, nobody waits (unless BG
			 * writes are queued for the GE's last lists already).
			 */
			GE_Stat[GE_ST_BG]++;
			WinDraw_GEKick();
		}
	}
	if (ge_inflight && ge_bgfly) {
		if (ge_nq == GE_NQ)
			ge_sync(GE_ST_BGQ_FULL);
		else {
			ge_qadr[ge_nq] = adr;
			ge_qdat[ge_nq] = data;
			ge_nq++;
			return;
		}
	}
	GE_BGWrite(adr, data);
}

/* list memory (see ge_mem) */
#define GE_VARENA	(1024 * 1024)	/* vertices and CLUTs */
#define GE_CLIST	(400 * 1024)	/* the lists */
#define GE_CBAND	12288		/* list bytes per band, at most */
#define GE_CFIXED	4096		/* ... and per list */
#define GE_VPAL		(2 * 256 * 4 + 32)	/* CLUT bytes per palette */

static int ge_vband(const GE_State *s);
static int ge_vline(const GE_State *s);

/* the lines left to the CPU, per reason (GE_R_*), since the last "ge" */
typedef struct {
	unsigned frames, frame;		/* frames with such lines; the last one + 1 */
	unsigned vmin, vmax;		/* VLINE range */
	unsigned dotx, doty;		/* the last one's screen size */
	BYTE	r28, vc0;		/* ... CRTC R20 and VC R0 */
} GE_CpuLine;
static GE_CpuLine ge_cpuline[GE_R_VLINE + 1];

int GE_Line(void)
{
	GE_State s;
	GE_Band *b;
	int r, vb, vl;

	r = VLINE >= GE_ROWS ? GE_R_VLINE : ge_eval(&s);
	if (r) {
		GE_CpuLine *c = &ge_cpuline[r];

		GE_StatCpuLines++;
		GE_Stat[GE_ST_CPU_REASON + r]++;
		/* what these lines are, for "ge" */
		if (!c->frames || VLINE < c->vmin)
			c->vmin = VLINE;
		if (VLINE > c->vmax)
			c->vmax = VLINE;
		if (c->frame != GE_Stat[GE_ST_FRAMES] + 1) {
			c->frame = GE_Stat[GE_ST_FRAMES] + 1;
			c->frames++;
		}
		c->r28 = CRTC_Regs[0x28];
		c->vc0 = VCReg0[1];
		c->dotx = TextDotX;
		c->doty = TextDotY;
		return 0;
	}
	if (ge_inflight && !(VLINE & 15))
		WinDraw_GEPoll();	/* done with the last frame? */
	vb = ge_vband(&s);
	vl = ge_vline(&s);
	if (ge_nband == GE_NBAND || ge_vneed + vb + vl + GE_VPAL > GE_VARENA)
		ge_sync(GE_ST_BANDS);	/* draws the bands, empties the snapshots */
	/* the waiting 65536 colour bands share the converted GVRAM (ge_g16): one palette */
	if (s.gm == 2 && s.ng && ge_g16pend && memcmp(ge_pal[ge_g16pal].regs, Pal_Regs, sizeof(ge_pal[0].regs)))
		ge_sync(GE_ST_PALS);

	if (ge_npal == 0 || GE_PalDirty) {
		GE_PalDirty = 0;
		if (ge_npal == 0 || memcmp(ge_pal[ge_palcur].text, TextPal, sizeof(ge_pal[0].text)) ||
		    memcmp(ge_pal[ge_palcur].grp, GrphPal, sizeof(ge_pal[0].grp)) ||
		    memcmp(ge_pal[ge_palcur].regs, Pal_Regs, sizeof(ge_pal[0].regs))) {
			if (ge_npal == GE_NPAL)
				ge_sync(GE_ST_PALS);
			ge_palcur = ge_npal++;
			memcpy(ge_pal[ge_palcur].text, TextPal, sizeof(ge_pal[0].text));
			memcpy(ge_pal[ge_palcur].grp, GrphPal, sizeof(ge_pal[0].grp));
			memcpy(ge_pal[ge_palcur].regs, Pal_Regs, sizeof(ge_pal[0].regs));
			ge_vneed += GE_VPAL;
		}
	}
	s.pal = ge_palcur;
	if (s.gm == 2 && s.ng) {
		ge_g16pend = 1;
		ge_g16pal = s.pal;
	}

	b = ge_nband ? &ge_band[ge_nband - 1] : NULL;
	if (b && b->y0 + b->h == (int)VLINE && memcmp(&b->st, &s, sizeof(s)) == 0) {
		b->h++;
		ge_mark_line(b, 0);
	} else {
		b = &ge_band[ge_nband++];
		b->st = s;
		b->y0 = VLINE;
		b->h = 1;
		ge_mark_line(b, 1);
		ge_vneed += vb;
	}
	ge_vneed += vl;
	GE_StatLines++;
	GE_Guard = 1;
	return 1;
}

/* ---- drawing ---- */

typedef struct {
	unsigned short u, v;
	short x, y, z;
} GE_TV;	/* textured sprite corner */

typedef struct {
	unsigned int c;
	short x, y, z, pad;
} GE_CV;	/* coloured sprite corner */

#define GE_TVFMT	(GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define GE_CVFMT	(GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)

static int ge_dotx;	/* width of the band being drawn */

/*
 * List memory.  sceGuStart() writes a list through the uncached alias
 * (| 0x40000000), so sceGuGetMemory() returns uncached memory: one bus write
 * per vertex field, made while the GE (started at every sceGuGetMemory /
 * sceGuDrawArray of a GU_DIRECT list) already reads main RAM for its copies.
 * The bands' lists are built as GU_CALL lists in ge_clist instead (no stall
 * address updates: the GE does not start before the list is complete), with
 * the vertices and CLUTs in ge_varena, cached; the caller writes the D-cache
 * back before it calls the list.  Both are reused once the GE is idle
 * (GE_Done); ge_vneed bounds what the waiting bands take of ge_varena.
 */
static BYTE ge_varena[GE_VARENA] __attribute__((aligned(64)));
static unsigned int ge_clist[GE_CLIST / 4] __attribute__((aligned(64)));

/* vertex / CLUT memory, 16-byte aligned */
static void *ge_mem(int size)
{
	void *p = ge_varena + ge_voff;

	ge_voff += (size + 15) & ~15;
	return p;
}

/*
 * The GE's texture cache is small: a wide sprite is drawn as strips at most
 * GE_STRIP texels wide (cut at multiples of GE_STRIP in u).
 */
#define GE_STRIP	32
#define GE_NSTRIP(w)	((w) / GE_STRIP + 2)	/* sprites a quad w wide can become */

/*
 * Upper bounds of the ge_varena bytes GE_Render takes for a band with state
 * s: per band (ge_vband), and per line (ge_vline): the BG tiles, 8 dots high
 * at least, a row of them per 8 lines and 2 more per band, for 2 planes, as
 * textured quads and as "gd" rectangles.
 */
static int ge_vtilerow(const GE_State *s)
{
	if (!((s->mode == GE_M || s->mode == GE_GM) && s->bgon))
		return 0;
	return 2 * ((s->dotx >> 3) + 1) * 2 * (sizeof(GE_TV) + sizeof(GE_CV));
}

static int ge_vband(const GE_State *s)
{
	int n = 16 + 2 * sizeof(GE_CV);		/* fill */

	if (s->mode == GE_G || s->mode == GE_GM)	/* ge_grp */
		n += s->ng * (16 + 2 * 2 * GE_NSTRIP(512) * 2 * sizeof(GE_TV) + 16 + 2 * 2 * sizeof(GE_TV));
	if (s->mode == GE_M || s->mode == GE_GM) {
		n += 16 + GE_NSTRIP(512) * 2 * sizeof(GE_TV);			/* composite */
		n += 4 * (16 + GE_NSTRIP(256) * 2 * sizeof(GE_TV));		/* ge_text */
		if (s->bgon)	/* sprites, tile batches (16 blocks each, aligned), "gd" */
			n += 128 * 2 * sizeof(GE_TV) + 5 * 16 * 16 + 2 * 16 + 2 * ge_vtilerow(s);
	}
	if (s->prio)	/* ge_prio draws the text / BG again, the layer, the fills */
		n = 2 * n + 4 * (16 + GE_NSTRIP(512) * 2 * sizeof(GE_TV));
	return n;
}

static int ge_vline(const GE_State *s)
{
	return (s->prio ? 2 : 1) * ((ge_vtilerow(s) + 7) / 8);
}

/* RGB565 value -> 8888 that the GE writes back as the same value */
static unsigned int ge_c32(DWORD w, unsigned int a)
{
	const unsigned int r = w & 31, g = (w >> 5) & 63, b = (w >> 11) & 31;

	return (a << 24) | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
}

/* coloured rectangle; with depth: also sets the depth to 0xffff (BG_PriBuf) */
static void ge_fill(int x, int y, int w, int h, unsigned int c, int depth)
{
	GE_CV *v = (GE_CV *)ge_mem(2 * sizeof(GE_CV));

	v[0].c = c;
	v[0].x = x;
	v[0].y = y;
	v[0].z = (short)0xffff;
	v[0].pad = 0;
	v[1].c = c;
	v[1].x = x + w;
	v[1].y = y + h;
	v[1].z = (short)0xffff;
	v[1].pad = 0;
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_COLOR_TEST);
	if (depth) {
		sceGuEnable(GU_DEPTH_TEST);
		sceGuDepthFunc(GU_ALWAYS);
	} else
		sceGuDisable(GU_DEPTH_TEST);
	sceGuDrawArray(GU_SPRITES, GE_CVFMT, 2, 0, v);
	GE_Stat[GE_ST_DRAWS]++;
	GE_Stat[GE_ST_VERTS] += 2;
	GE_Stat[GE_ST_PIXELS] += w * h;
	sceGuEnable(GU_TEXTURE_2D);
}


static GE_TV *ge_sprite(GE_TV *t, int x, int y, int w, int h, int u, int v, int hf, int vf, int z)
{
	t[0].u = hf ? u + w : u;
	t[1].u = hf ? u : u + w;
	t[0].v = vf ? v + h : v;
	t[1].v = vf ? v : v + h;
	t[0].x = x;
	t[1].x = x + w;
	t[0].y = y;
	t[1].y = y + h;
	t[0].z = t[1].z = z;
	return t + 2;
}

/*
 * One textured quad: screen x, y, w, h from texture u, v (w x h), mirrored
 * when hf / vf, clipped to 0 <= x < ge_dotx; unmirrored wide quads become
 * strips (GE_NSTRIP(w) sprites at most).
 */
static GE_TV *ge_quad(GE_TV *t, int x, int y, int w, int h, int u, int v, int hf, int vf, int z)
{
	if (x < 0) {
		if (!hf)
			u -= x;
		w += x;
		x = 0;
	}
	if (x + w > ge_dotx) {
		const int cut = x + w - ge_dotx;

		if (hf)
			u += cut;
		w -= cut;
	}
	if (w <= 0 || h <= 0)
		return t;
	GE_Stat[GE_ST_PIXELS] += w * h;
	if (hf || w <= GE_STRIP)
		return ge_sprite(t, x, y, w, h, u, v, hf, vf, z);
	while (w > 0) {
		int sw = GE_STRIP - (u & (GE_STRIP - 1));

		if (sw > w)
			sw = w;
		t = ge_sprite(t, x, y, sw, h, u, v, 0, vf, z);
		x += sw;
		u += sw;
		w -= sw;
	}
	return t;
}

static void ge_draw(GE_TV *start, GE_TV *end)
{
	if (end > start)
	{
		sceGuDrawArray(GU_SPRITES, GE_TVFMT, end - start, 0, start);
		GE_Stat[GE_ST_DRAWS]++;
		GE_Stat[GE_ST_VERTS] += end - start;
	}
}

/*
 * Quads sorted by palette block (16 CLUT offsets): made in one walk into
 * ge_sq (tagged with their block in ge_sb), then copied into the list
 * memory block by block (in order: the same quads as a walk per block).
 */
#define GE_MAXQ		(65 * 34 + 128)	/* tiles of a plane: 512 dots, 256 lines of 8x8; or sprites */
static GE_TV ge_sq[GE_MAXQ * 2];

/*
 * ge_prio: the layer's draws only mark the depth buffer (the colour is
 * masked): what they draw gets their z (not 0xffff), whatever was there.
 */
static int ge_zmask;

/* the depth test as the layer's draws without one need it */
static void ge_nodepth(void)
{
	if (ge_zmask) {
		sceGuEnable(GU_DEPTH_TEST);
		sceGuDepthFunc(GU_ALWAYS);
	} else
		sceGuDisable(GU_DEPTH_TEST);
}
static BYTE ge_sb[GE_MAXQ];
static GE_CV ge_cq[GE_MAXQ * 2];

/* draw the nq quads of ge_sq/ge_sb, n[blk] of each block */
/*
 * The layer pass's BG/sprite draws of a band that ge_prio masks with them
 * (pafter & 2) are recorded and drawn again from the same vertices.
 */
typedef struct {
	const void *v;
	const void *tex;	/* textured: the atlas, else NULL ("gd" rectangles) */
	short	n, th;		/* vertices, the atlas' height */
	BYTE	blk;		/* palette block */
} GE_Rec;

#define GE_NREC		1024
static GE_Rec ge_rec[GE_NREC];
static int ge_nrec, ge_recon, ge_recok;
static const void *ge_rtex;	/* the atlas set for the draws */
static int ge_rth;
static unsigned ge_buildno = 1;	/* GE_Build calls: the vertices are the build's */

static void ge_record(const void *tex, const void *v, int n, int blk)
{
	GE_Rec *r;

	if (!ge_recon)
		return;
	if (ge_nrec == GE_NREC) {
		ge_recok = 0;
		return;
	}
	r = &ge_rec[ge_nrec++];
	r->v = v;
	r->tex = tex;
	r->n = n;
	r->th = ge_rth;
	r->blk = blk;
}

static void ge_sorted_draw(int nq, const int *n)
{
	GE_TV *base[16], *cur[16];
	int k, q;

	for (k = 0; k < 16; k++)
		if (n[k])
			base[k] = cur[k] = (GE_TV *)ge_mem(n[k] * 2 * sizeof(GE_TV));
	for (q = 0; q < nq; q++) {
		GE_TV *d = cur[ge_sb[q]];

		d[0] = ge_sq[q * 2];
		d[1] = ge_sq[q * 2 + 1];
		cur[ge_sb[q]] = d + 2;
	}
	for (k = 0; k < 16; k++)
		if (n[k]) {
			sceGuClutMode(GU_PSM_8888, 0, 0x0f, k);
			ge_draw(base[k], cur[k]);
			ge_record(ge_rtex, base[k], cur[k] - base[k], k);
		}
}

/*
 * The tiles of a BG plane on the band, as bg_drawline_loopx8/16: per line,
 * (TextDotX >> 3 or 4) + 1 tiles from x = -(scroll & (size - 1)).
 * mode 0: textured quads (ge_sq), 1: the "gd" rectangles (palette block
 * != 0, runs of one colour merged, into ge_cq), 2: note what they use.
 * Returns the number of quads / rectangles.
 */
static inline __attribute__((always_inline))
int ge_tiles_sz(const GE_Band *b, const int sz, const int mode, DWORD top, DWORD scx, DWORD scy, DWORD adj,
		const GE_Pal *pal, int *n)
{
	const GE_State *s = &b->st;
	const int sh = (sz == 8) ? 3 : 4;
	const int ncol = (s->dotx >> sh) + 1, dotx = s->dotx;
	const DWORD sy = scy + s->lbase + b->y0;	/* scroll + VLINEBG - BG_VLINE */
	const DWORD sx = scx - adj;
	const int x00 = -(int)(sx & (sz - 1));
	unsigned int c32[16];
	GE_TV *t = ge_sq;
	GE_CV *c = ge_cq;
	int i = 0, nq = 0, px = 0;

	if (mode == 1)
		for (i = 1; i < 16; i++)
			c32[i] = ge_c32(pal->text[i * 16], 255);
	i = 0;
	while (i < b->h) {
		const DWORD yy = sy + i;
		const int r = yy & (sz - 1);
		const int nn = (sz - r < b->h - i) ? sz - r : b->h - i;
		const int y0 = b->y0 + i;
		const BYTE *map = BG + top + ((sz == 8) ? ((yy & 0x1f8) << 4) : ((yy & 0x3f0) << 3));
		DWORD col = (sx >> sh) & 63;
		int x = x00, k, run = 0;

		for (k = 0; k < ncol; k++, x += sz, col = (col + 1) & 63) {
			const DWORD bl = map[col * 2], pat = map[col * 2 + 1];
			const int blk = bl & 15;

			if (mode == 2) {
				GE_SET(ge_usemap, (DWORD)(map + col * 2 - BG) >> 1);
				if (sz == 8)
					GE_SET(ge_use8, pat);
				else
					GE_SET(ge_use16, pat);
			} else if (mode == 1) {
				const int x0 = x < 0 ? 0 : x, x1 = x + sz > dotx ? dotx : x + sz;

				if (!blk)
					run = 0;
				else if (run == blk && c[-1].x == x0)
					c[-1].x = x1;	/* the same colour goes on */
				else {
					c[0].c = c[1].c = c32[blk];
					c[0].x = x0;
					c[1].x = x1;
					c[0].y = y0;
					c[1].y = y0 + nn;
					c[0].z = c[1].z = 0;
					c[0].pad = c[1].pad = 0;
					c += 2;
					nq++;
					run = blk;
				}
			} else {
				/* as ge_quad: clipped to 0 <= x < dotx, at most 16 wide: no strips */
				const int hf = (bl & 0x40) != 0, vf = (bl & 0x80) != 0;
				const int pv = (sz == 8) ? (pat >> 6) * 8 : (pat >> 5) * 16;
				const int v = vf ? pv + sz - r - nn : pv + r;
				int u = (sz == 8) ? (pat & 63) * 8 : (pat & 31) * 16;
				int xx = x, w = sz;

				if (xx < 0) {
					if (!hf)
						u -= xx;
					w += xx;
					xx = 0;
				}
				if (xx + w > dotx) {
					if (hf)
						u += xx + w - dotx;
					w = dotx - xx;
				}
				if (w <= 0)
					continue;
				px += w * nn;
				t[0].u = hf ? u + w : u;
				t[1].u = hf ? u : u + w;
				t[0].v = vf ? v + nn : v;
				t[1].v = vf ? v : v + nn;
				t[0].x = xx;
				t[1].x = xx + w;
				t[0].y = y0;
				t[1].y = y0 + nn;
				t[0].z = t[1].z = 0;
				t += 2;
				ge_sb[nq++] = blk;
				n[blk]++;
			}
		}
		i += nn;
	}
	GE_Stat[GE_ST_PIXELS] += px;
	return nq;
}

static int ge_tiles(const GE_Band *b, int sz, int mode, DWORD top, DWORD scx, DWORD scy, DWORD adj,
		    const GE_Pal *pal, int *n)
{
	if (sz == 8)
		return ge_tiles_sz(b, 8, mode, top, scx, scy, adj, pal, n);
	return ge_tiles_sz(b, 16, mode, top, scx, scy, adj, pal, n);
}

/*
 * The sprites shown on the band (Sprite_CollectLine), per priority 1-3,
 * from sprite 127 down to 0, into lst[level - 1][]; marking: note what
 * they use instead.
 */
static void ge_sprites(const GE_Band *b, BYTE lst[3][128], int *cnt, int marking)
{
	const GE_State *s = &b->st;
	const WORD *sr = (const WORD *)(s->spr == GE_LIVE ? Sprite_Regs : ge_spr[s->spr]);
	const DWORD l0 = s->lbase + b->y0;	/* VLINEBG - BG_VLINE on the first line */
	int n;

	for (n = 127; n >= 0; n--) {
		const WORD *sp = sr + n * 4;
		const int level = sp[3] & 3;
		int top;

		if (!level)
			continue;
		if (((sp[0] + s->hadj) & 0x3ff) >= (DWORD)s->dotx + 16)
			continue;
		/* line i of the band shows row i - top, if 0-15 */
		top = (int)((DWORD)(sp[1] & 0x3ff) - l0 - 1) - 15;
		if (top >= b->h || top + 16 <= 0)
			continue;
		if (marking)
			GE_SET(ge_use16, sp[2] & 0xff);
		else
			lst[level - 1][cnt[level - 1]++] = n;
	}
}

/* the sprites of priority 'level' on the band, from ge_sprites' list */
static void ge_spritelevel(const GE_Band *b, const BYTE *lst, int cnt)
{
	const GE_State *s = &b->st;
	const WORD *sr = (const WORD *)(s->spr == GE_LIVE ? Sprite_Regs : ge_spr[s->spr]);
	const DWORD l0 = s->lbase + b->y0;
	int n[16], j, nq = 0;
	GE_TV *t = ge_sq;

	if (!cnt)
		return;
	memset(n, 0, sizeof(n));
	for (j = 0; j < cnt; j++) {
		const int sn = lst[j];
		const WORD *sp = sr + sn * 4;
		const DWORD ctrl = sp[2];
		const int top = (int)((DWORD)(sp[1] & 0x3ff) - l0 - 1) - 15;
		const int i0 = top < 0 ? 0 : top, i1 = top + 16 > b->h ? b->h : top + 16;
		const int pv = ((ctrl >> 5) & 7) * 16, vf = (ctrl & 0x8000) != 0;
		GE_TV *e = ge_quad(t, (int)((sp[0] + s->hadj) & 0x3ff) - 16, b->y0 + i0, 16, i1 - i0,
				   (ctrl & 31) * 16, vf ? pv + 16 - (i0 - top) - (i1 - i0) : pv + (i0 - top),
				   (ctrl & 0x4000) != 0, vf, sn * 256);

		if (e > t) {
			const int blk = (ctrl >> 8) & 15;

			t = e;
			ge_sb[nq++] = blk;
			n[blk]++;
		}
	}
	sceGuTexImage(0, 512, 128, 512, GE_VRAM(GE_ATLAS16));
	ge_rtex = GE_VRAM(GE_ATLAS16);
	ge_rth = 128;
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthFunc(ge_zmask ? GU_ALWAYS : GU_LEQUAL);
	ge_sorted_draw(nq, n);
}

static void ge_bgplane(const GE_Band *b, int sz, DWORD top, DWORD scx, DWORD scy, DWORD adj)
{
	int n[16], nq;

	memset(n, 0, sizeof(n));
	nq = ge_tiles(b, sz, 0, top, scx, scy, adj, NULL, n);
	if (sz == 8)
		sceGuTexImage(0, 512, 32, 512, GE_VRAM(GE_ATLAS8));
	else
		sceGuTexImage(0, 512, 128, 512, GE_VRAM(GE_ATLAS16));
	ge_rtex = GE_VRAM(sz == 8 ? GE_ATLAS8 : GE_ATLAS16);
	ge_rth = sz == 8 ? 32 : 128;
	ge_nodepth();
	ge_sorted_draw(nq, n);
}

static void ge_under(const GE_Band *b, int sz, DWORD top, DWORD scx, DWORD scy, DWORD adj, const GE_Pal *pal)
{
	const int n = ge_tiles(b, sz, 1, top, scx, scy, adj, pal, NULL);
	GE_CV *v;

	if (!n)
		return;
	v = (GE_CV *)ge_mem(n * 2 * sizeof(GE_CV));
	memcpy(v, ge_cq, n * 2 * sizeof(GE_CV));
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_ALPHA_TEST);
	ge_nodepth();
	sceGuDrawArray(GU_SPRITES, GE_CVFMT, n * 2, 0, v);
	ge_record(NULL, v, n * 2, 0);
	GE_Stat[GE_ST_DRAWS]++;
	GE_Stat[GE_ST_VERTS] += n * 2;
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_ALPHA_TEST);
}

/* BG_DrawLine without the fill: sprites and BG planes */
/* "ge": CPU time of the parts of the layer build */
static unsigned ge_lt;

static void ge_ltick(int k)
{
	unsigned t;

	if (ge_zmask)
		return;		/* ge_prio (the screen) draws the BG again */
	t = sceKernelGetSystemTimeLow();
	GE_Stat[GE_ST_LAYER_US + k] += t - ge_lt;
	ge_lt = t;
}

static void ge_bg(const GE_Band *b, int gd, const GE_Pal *pal)
{
	const GE_State *s = &b->st;
	const int bg1 = s->chr8 && (s->bg9 & 8), bg0 = s->bg9 & 1;
	const int sz0 = s->chr8 ? 8 : 16;
	/* the original passed no H adjust for 16x16 without graphics */
	const DWORD adj0 = (s->chr8 || gd) ? s->hadj : 0;
	BYTE lst[3][128];
	int cnt[3] = { 0, 0, 0 };

	if (gd) {
		/* under everything: BG0's, then BG1's (BG1 comes first, wins) */
		if (bg0)
			ge_under(b, sz0, s->bg0top, s->bg0sx, s->bg0sy, adj0, pal);
		if (bg1)
			ge_under(b, 8, s->bg1top, s->bg1sx, s->bg1sy, s->hadj, pal);
	}
	ge_ltick(1);
	sceGuTexMode(GU_PSM_T4, 0, 0, 0);
	sceGuEnable(GU_ALPHA_TEST);
	ge_sprites(b, lst, cnt, 0);
	ge_spritelevel(b, lst[0], cnt[0]);
	if (bg1)
		ge_bgplane(b, 8, s->bg1top, s->bg1sx, s->bg1sy, s->hadj);
	ge_spritelevel(b, lst[1], cnt[1]);
	if (bg0)
		ge_bgplane(b, sz0, s->bg0top, s->bg0sx, s->bg0sy, adj0);
	ge_spritelevel(b, lst[2], cnt[2]);
	ge_ltick(2);
}

/* ge_bg in the layer pass; the draws are recorded when ge_prio draws them again */
static void ge_bg_rec(GE_Band *b, int gd)
{
	const int rec = b->st.prio && (b->st.pafter & 2);

	b->recbuild = 0;
	if (rec) {
		ge_recon = 1;
		ge_recok = 1;
		b->rec0 = ge_nrec;
	}
	ge_bg(b, gd, &ge_pal[b->st.pal]);
	if (rec) {
		ge_recon = 0;
		b->rec1 = ge_nrec;
		if (ge_recok)
			b->recbuild = ge_buildno;
	}
}

/* what the waiting bands use of BG[] (GE_BGData) */
static void ge_mark_bg(void)
{
	int i;

	memset(ge_use16, 0, sizeof(ge_use16));
	memset(ge_use8, 0, sizeof(ge_use8));
	memset(ge_usemap, 0, sizeof(ge_usemap));
	for (i = 0; i < ge_nband; i++) {
		const GE_Band *b = &ge_band[i];
		const GE_State *s = &b->st;

		if (!((s->mode == GE_M || s->mode == GE_GM) && s->bgon))
			continue;
		ge_dotx = s->dotx;
		if (s->chr8 && (s->bg9 & 8))
			ge_tiles(b, 8, 2, s->bg1top, s->bg1sx, s->bg1sy, s->hadj, NULL, NULL);
		if (s->bg9 & 1)
			ge_tiles(b, s->chr8 ? 8 : 16, 2, s->bg0top, s->bg0sx, s->bg0sy,
				 (s->chr8 || s->mcase) ? s->hadj : 0, NULL, NULL);
		ge_sprites(b, NULL, NULL, 1);
	}
	ge_usevalid = 1;
	GE_Stat[GE_ST_BG_SCAN]++;
}

/*
 * Copies in VRAM of what the bands read from main RAM (texturing from main
 * RAM is slow), made by the GE at the start of the list:
 *  - GVRAM: rows (gy + y) & 511 of the first page drawn, all 512 words, at
 *    row y of GE_GCOPY: the rows of ScrBufL from 256 on, unused while the
 *    screen is at most 256 lines high (CPU lines there wait for the GE in
 *    psp_flush_line, and are not shown).  Pages with another Y scroll, and
 *    the odd dot (row - 1), still read GVRAM itself.
 *  - text: TextDrawWork rows (ty + y) & 1023, 512 dots from tx & ~15, at
 *    row y of GE_TCOPY.
 */
#define GE_GCOPY	(GE_SCRBUF_L + 256 * 1024)
#define GE_TCOPY	0x001da000	/* 512 x 256 bytes, up to 0x1fa000 */

static int ge_gcopy_ok(const GE_State *s, int p)
{
	return s->gm != 2 && TextDotY <= 256 && s->ng && s->gy[p] == s->gy[s->gpage[0]];
}

static int ge_tcopy_x(const GE_State *s)	/* first dot copied, or -1 */
{
	const int ws = (s->tx & ~15) > 512 ? 512 : (s->tx & ~15);

	return (s->tx - ws) + s->dotx <= 512 ? ws : -1;
}

/* the 32-word columns of GVRAM words a .. e - 1, as bits */
static unsigned ge_chunks(int a, int e)
{
	return ((1u << ((e + 31) >> 5)) - 1) & ~((1u << (a >> 5)) - 1);
}

/* the 32-word columns of the GVRAM rows the pages textured from the copy read (ge_grp) */
static unsigned ge_gcopy_cols(const GE_State *s)
{
	unsigned m = 0;
	int k;

	for (k = 0; k < s->ng; k++) {
		const int p = s->gpage[k];
		const int x = s->gx[p];
		const int skip = !s->gm;	/* the odd dot (ge_grp) */
		int n1 = 512 - skip - x;

		if (!ge_gcopy_ok(s, p))
			continue;
		if (n1 > s->dotx)
			n1 = s->dotx;
		if (n1 > 0)
			m |= ge_chunks(x, x + n1);
		if (n1 + skip < s->dotx)
			m |= ge_chunks(0, s->dotx - n1 - skip);
	}
	return m;
}

/*
 * What the copies hold, per row y: the source row, its generation (GE_GRowGen,
 * GE_TRowGen, bumped by every write to it) and the columns copied.  The rows
 * and columns that still hold what the band reads are not copied again: a
 * still graphic or text screen is not copied at all, one that scrolls
 * sideways only for the columns that come into view.  (A write between the
 * build and the copy may leave a newer row than recorded: copied again.)
 */
DWORD GE_GRowGen[512], GE_TRowGen[1024], GE_GGenAll, GE_TGenAll;

typedef struct {
	DWORD	gen, all;
	WORD	src;
	WORD	cols;		/* 32-word columns held, 0: nothing */
} GE_GCopy;

typedef struct {
	DWORD	gen, all;
	WORD	src, ws;	/* bytes ws .. ws + 2 * w - 1 of row src */
	WORD	w;		/* 0: nothing */
} GE_TCopy;

static GE_GCopy ge_gc[256];
static GE_TCopy ge_tc[256];

void GE_ScrRowWritten(DWORD y)
{
	if (y >= 256 && y < 512)
		ge_gc[y - 256].cols = 0;	/* the GVRAM copy's row */
}

/* the columns row y misses of need (bit 16: it holds nothing of row src) */
static unsigned ge_gmiss(int y, int src, unsigned need)
{
	const GE_GCopy *g = &ge_gc[y];

	if (g->cols && g->src == src && g->gen == GE_GRowGen[src] && g->all == GE_GGenAll)
		return need & ~g->cols;
	return need | 0x10000;
}

static int ge_tmiss(int y, int src, int ws, int w)
{
	const GE_TCopy *t = &ge_tc[y];

	return !(t->w >= w && t->src == src && t->ws == ws && t->gen == GE_TRowGen[src] && t->all == GE_TGenAll);
}

static void ge_copy(const GE_Band *b)
{
	const GE_State *s = &b->st;
	int i, k, n;

	if ((s->mode == GE_G || s->mode == GE_GM) && ge_gcopy_ok(s, s->gpage[0])) {
		/* the columns read only (a 256 dot screen reads half of them) */
		const unsigned need = ge_gcopy_cols(s);
		const int gy = s->gy[s->gpage[0]];

		for (i = 0; i < b->h; i += n) {
			const int y = b->y0 + i, row = (gy + y) & 511;
			const unsigned miss = ge_gmiss(y, row, need), m = miss & 0xffff;
			int c0, c1;

			/* the rows after it that miss the same, up to the wrap */
			for (n = 1; i + n < b->h && row + n < 512 && ge_gmiss(y + n, row + n, need) == miss; n++)
				;
			for (k = 0; k < n; k++) {
				GE_GCopy *g = &ge_gc[y + k];

				g->cols = (miss & 0x10000) ? need : g->cols | need;
				g->src = row + k;
				g->gen = GE_GRowGen[row + k];
				g->all = GE_GGenAll;
			}
			for (c0 = 0; c0 < 16; c0 = c1) {
				if (!(m & (1u << c0))) {
					c1 = c0 + 1;
					continue;
				}
				for (c1 = c0 + 1; c1 < 16 && (m & (1u << c1)); c1++)
					;
				sceGuCopyImage(GU_PSM_5650, c0 * 32, row, (c1 - c0) * 32, n, 512, GVRAM,
					       c0 * 32, y, 512, GE_VRAM(GE_GCOPY));
				GE_Stat[GE_ST_COPY_BYTES] += n * (c1 - c0) * 64;
			}
		}
	}
	if ((s->mode == GE_M || s->mode == GE_GM) && s->ton && ge_tcopy_x(s) >= 0) {
		const int ws = ge_tcopy_x(s);
		/* the bytes read (ge_text): tx - ws .. tx - ws + dotx - 1, as 16-bit dots */
		const int w = (s->tx - ws + s->dotx + 1) >> 1;

		for (i = 0; i < b->h; i += n) {
			const int y = b->y0 + i, row = (s->ty + y) & 1023;
			const int miss = ge_tmiss(y, row, ws, w);

			for (n = 1; i + n < b->h && row + n < 1024 && ge_tmiss(y + n, row + n, ws, w) == miss; n++)
				;
			if (!miss)
				continue;
			for (k = 0; k < n; k++) {
				GE_TCopy *t = &ge_tc[y + k];

				t->src = row + k;
				t->ws = ws;
				t->w = w;
				t->gen = GE_TRowGen[row + k];
				t->all = GE_TGenAll;
			}
			sceGuCopyImage(GU_PSM_5650, ws / 2, row, w, n, 512, TextDrawWork, 0, y, 256,
				       GE_VRAM(GE_TCOPY));
			GE_Stat[GE_ST_COPY_BYTES] += n * w * 2;
		}
	}
}

/* Text_DrawLine: TextDrawWork, 1 byte per dot, as a T8 texture */
static void ge_text(const GE_Band *b, int opaque)
{
	const GE_State *s = &b->st;
	const int ws = ge_tcopy_x(s);
	GE_TV *v, *e;
	int i = 0;

	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
	ge_nodepth();
	if (opaque)
		sceGuDisable(GU_ALPHA_TEST);
	else
		sceGuEnable(GU_ALPHA_TEST);
	if (ws >= 0) {
		v = (GE_TV *)ge_mem(GE_NSTRIP(512) * 2 * sizeof(GE_TV));
		sceGuTexImage(0, 512, 256, 512, GE_VRAM(GE_TCOPY));
		ge_draw(v, ge_quad(v, 0, b->y0, s->dotx, b->h, s->tx - ws, b->y0, 0, 0, 0));
		return;
	}
	while (i < b->h) {
		const int row = (s->ty + b->y0 + i) & 1023;
		const int n = (1024 - row < b->h - i) ? 1024 - row : b->h - i;
		int c0;

		for (c0 = 0; c0 < s->dotx; c0 += 256) {
			const int x = s->tx + c0;
			const int w = (s->dotx - c0 < 256) ? s->dotx - c0 : 256;

			v = e = (GE_TV *)ge_mem(GE_NSTRIP(256) * 2 * sizeof(GE_TV));
			sceGuTexImage(0, 512, 512, 1024, TextDrawWork + row * 1024 + (x & ~15));
			e = ge_quad(e, c0, b->y0 + i, w, n, x & 15, 0, 0, 0, 0);
			ge_draw(v, e);
		}
		i += n;
	}
}

/* the graphic pages of a band (Grp_DrawLine4), into the screen texture */
/*
 * 65536 colours.  Grp_DrawLine16 maps each GVRAM word through two palette
 * register bytes and Pal16 (Grp16_Col), which no CLUT can do: the CPU
 * converts the GVRAM rows the bands show into ge_g16 (main RAM, the final
 * RGB565 dots, row for row), and the GE copies them into the screen.  As
 * the GE's GVRAM copies, the rows remember the GVRAM row generation, the
 * palette tables (ge_g16tab) and the 32-word columns converted: only
 * what changed is converted again.  The waiting 65536 colour bands share
 * one palette (GE_Line) and are not handed to the GE while it may still
 * copy rows of the last ones (GE_CanKick).
 */
DWORD GE_Pal16Gen;
static WORD ge_g16[512 * 512] __attribute__((aligned(64)));
static DWORD ge_g16gen[512], ge_g16all[512], ge_g16tg[512];
static WORD ge_g16cols[512];
static WORD ge_g16lo[256], ge_g16hi[256];	/* Pal_Regs[Pal16Adr[lo]], Pal_Regs[Pal16Adr[hi] + 2] << 8 */
static BYTE ge_g16regs[512];
static DWORD ge_g16tab = 1, ge_g16p16 = (DWORD)-1;

/* 65536 colours, one dot (Grp16_Col with the tables) */
#define GE_G16DOT(w)	((w) ? Pal16[ge_g16lo[(w) & 0xff] | ge_g16hi[(w) >> 8]] : 0)

/*
 * GVRAM_Write (GE_GVRAM_ROW): the word at byte offset a changed.  A row
 * that holds converted dots stays valid: the word is converted again
 * (if its columns were converted), its generation follows.
 */
int GE_G16Live;

void GE_G16Write(DWORD a)
{
	const int row = (a >> 10) & 511;
	const DWORD g = GE_GRowGen[row]++;

	if (ge_g16gen[row] == g && ge_g16tg[row] == ge_g16tab && ge_g16all[row] == GE_GGenAll) {
		const int x = (a >> 1) & 511;

		ge_g16gen[row] = g + 1;
		if (ge_g16cols[row] & (1u << (x >> 5))) {
			const DWORD w = ((const WORD *)GVRAM)[row * 512 + x];

			ge_g16[row * 512 + x] = GE_G16DOT(w);
		}
	}
}

/* GVRAM bytes a .. a + n - 1 (words) changed (GVRAM_FastClear): GE_G16Write for each word */
void GE_GvramSpan(DWORD a, DWORD n)
{
	DWORD e = a + n;

	if (e > 0x80000)
		e = 0x80000;
	while (a < e) {
		const int row = (a >> 10) & 511;
		const DWORD re = ((a | 1023) + 1 < e) ? (a | 1023) + 1 : e;
		const DWORD g = GE_GRowGen[row]++;

		if (GE_G16Live && ge_g16gen[row] == g && ge_g16tg[row] == ge_g16tab &&
		    ge_g16all[row] == GE_GGenAll) {
			const WORD *src = (const WORD *)GVRAM + row * 512;
			WORD *d = ge_g16 + row * 512;
			const unsigned cols = ge_g16cols[row];
			DWORD x;

			ge_g16gen[row] = g + 1;
			for (x = (a >> 1) & 511; x <= ((re - 1) >> 1 & 511); x++)
				if (cols & (1u << (x >> 5))) {
					const DWORD w = src[x];

					d[x] = GE_G16DOT(w);
				}
		}
		a = re;
	}
}

static void ge_g16_table(const GE_Pal *pal)
{
	extern WORD Pal16Adr[256];
	int i;

	if (ge_g16p16 == GE_Pal16Gen && !memcmp(ge_g16regs, pal->regs, sizeof(ge_g16regs)))
		return;
	memcpy(ge_g16regs, pal->regs, sizeof(ge_g16regs));
	ge_g16p16 = GE_Pal16Gen;
	ge_g16tab++;
	for (i = 0; i < 256; i++) {
		ge_g16lo[i] = pal->regs[Pal16Adr[i]];
		ge_g16hi[i] = pal->regs[Pal16Adr[i] + 2] << 8;
	}
}

static void ge_grp16(const GE_Band *b)
{
	const GE_State *s = &b->st;
	const int x = s->gx[0];
	/* Grp_DrawLine16: dots x .. 511 of the line, then 0, 1, ... */
	const int n1 = 512 - x < s->dotx ? 512 - x : s->dotx;
	const unsigned need = ge_chunks(x, x + n1) | (n1 < s->dotx ? ge_chunks(0, s->dotx - n1) : 0);
	int i, n, k, c;

	ge_g16_table(&ge_pal[s->pal]);
	GE_G16Live = 1;		/* GVRAM_Write converts the words of valid rows */
	for (i = 0; i < b->h; i++) {
		const int row = (s->gy[0] + b->y0 + i) & 511;
		unsigned miss;

		if (ge_g16gen[row] == GE_GRowGen[row] && ge_g16all[row] == GE_GGenAll && ge_g16tg[row] == ge_g16tab) {
			miss = need & ~ge_g16cols[row];
			if (miss)
				GE_Stat[GE_ST_G16_WHY + 4]++;	/* columns not converted yet */
		} else {
			/* why the row is converted again */
			GE_Stat[GE_ST_G16_WHY + (!ge_g16cols[row] ? 0 : ge_g16tg[row] != ge_g16tab ? 1 :
						  ge_g16all[row] != GE_GGenAll ? 2 : 3)]++;
			miss = need;
			ge_g16cols[row] = 0;
		}
		if (!miss)
			continue;
		for (c = 0; c < 16; c++)
			if (miss & (1u << c)) {
				const WORD *src = (const WORD *)GVRAM + row * 512 + c * 32;
				WORD *d = ge_g16 + row * 512 + c * 32;

				for (k = 0; k < 32; k += 4) {
					const DWORD w0 = src[k], w1 = src[k + 1], w2 = src[k + 2], w3 = src[k + 3];

					d[k] = GE_G16DOT(w0);
					d[k + 1] = GE_G16DOT(w1);
					d[k + 2] = GE_G16DOT(w2);
					d[k + 3] = GE_G16DOT(w3);
				}
				GE_Stat[GE_ST_G16_DOTS] += 32;
			}
		ge_g16cols[row] |= miss;
		ge_g16gen[row] = GE_GRowGen[row];
		ge_g16all[row] = GE_GGenAll;
		ge_g16tg[row] = ge_g16tab;
	}
	/* into the screen, the rows up to the wrap at a time */
	for (i = 0; i < b->h; i += n) {
		const int row = (s->gy[0] + b->y0 + i) & 511;

		n = (512 - row < b->h - i) ? 512 - row : b->h - i;
		sceGuCopyImage(GU_PSM_5650, x, row, n1, n, 512, ge_g16, 0, b->y0 + i, 512, GE_VRAM(GE_SCRBUF_L));
		if (n1 < s->dotx)
			sceGuCopyImage(GU_PSM_5650, 0, row, s->dotx - n1, n, 512, ge_g16, n1, b->y0 + i, 512,
				       GE_VRAM(GE_SCRBUF_L));
		GE_Stat[GE_ST_PIXELS] += n * s->dotx;
	}
	sceGuTexSync();		/* the copies are done before the layer goes over them */
}

static void ge_grp(const GE_Band *b, const unsigned int *gclut)
{
	const GE_State *s = &b->st;
	/* 256 colours: no odd dot, the line wraps to its dot 0 (Grp_DrawLine8) */
	const int skip = !s->gm;
	int k;

	sceGuClutLoad(s->gm ? 256 / 8 : 16 / 8, gclut);
	sceGuTexMode(GU_PSM_T16, 0, 0, 0);
	sceGuDisable(GU_DEPTH_TEST);
	for (k = 0; k < s->ng; k++) {
		const int p = s->gpage[k];
		const int x = s->gx[p];
		const int copy = ge_gcopy_ok(s, p);
		/*
		 * Grp_DrawLine4: dots x .. 510 of the line, then the dot at 511 of
		 * the line above (GVRAM word row * 512 - 1), then 0, 1, ...
		 */
		int n1 = 512 - skip - x;
		GE_TV *v, *e;
		int i, n;

		if (n1 > s->dotx)
			n1 = s->dotx;
		if (s->gm)
			sceGuClutMode(GU_PSM_8888, p * 8, 0xff, 0);
		else
			sceGuClutMode(GU_PSM_8888, p * 4, 0x0f, 0);
		if (k == 0)
			sceGuDisable(GU_ALPHA_TEST);
		else
			sceGuEnable(GU_ALPHA_TEST);

		v = e = (GE_TV *)ge_mem(2 * 2 * GE_NSTRIP(512) * 2 * sizeof(GE_TV));
		for (i = 0; i < b->h; i += n) {
			/* the copy has the row of line y at y */
			const int row = copy ? b->y0 : (s->gy[p] + b->y0 + i) & 511;

			n = copy ? b->h : ((512 - row < b->h - i) ? 512 - row : b->h - i);
			e = ge_quad(e, 0, b->y0 + i, n1, n, x, row, 0, 0, 0);
			if (n1 + skip < s->dotx)
				e = ge_quad(e, n1 + skip, b->y0 + i, s->dotx - n1 - skip, n, 0, row, 0, 0, 0);
		}
		if (copy)
			sceGuTexImage(0, 512, 256, 512, GE_VRAM(GE_GCOPY));
		else
			sceGuTexImage(0, 512, 512, 512, GVRAM);
		ge_draw(v, e);

		if (skip && n1 < s->dotx) {
			/* the odd dot: GVRAM - 1 line as the texture, so that row 0 reads GVRAM[-2] too */
			v = e = (GE_TV *)ge_mem(2 * 2 * sizeof(GE_TV));
			for (i = 0; i < b->h; i += n) {
				const int row = (s->gy[p] + b->y0 + i) & 511;

				n = (512 - row < b->h - i) ? 512 - row : b->h - i;
				e = ge_quad(e, n1, b->y0 + i, 1, n, 511, row, 0, 0, 0);
			}
			sceGuTexImage(0, 512, 512, 512, (const void *)((unsigned int)GVRAM - 1024));
			ge_draw(v, e);
		}
	}
}

/* the layer over the band's lines of the screen, at depth z */
static void ge_layer_quad(const GE_Band *b, int z)
{
	GE_TV *v = (GE_TV *)ge_mem(GE_NSTRIP(512) * 2 * sizeof(GE_TV));

	sceGuTexMode(GU_PSM_5650, 0, 0, 0);
	sceGuTexImage(0, 512, 256, 512, GE_VRAM(GE_LAYER));
	ge_draw(v, ge_quad(v, 0, b->y0, b->st.dotx, b->h, 0, b->y0, 0, 0, z));
}

/*
 * Graphics between text and BG (DrawLine, ge_eval): the screen holds the
 * graphics (Grp_LineBuf); the line is
 *	the layer where a step after the graphics draws it (pafter), else
 *	the graphics where they are not 0, else
 *	the layer if it came before them (pbefore), else the graphics.
 * The depth buffer (free after the layer pass) holds the masks: 0xffff
 * cleared (the colour masked), then z 0 where the graphics are not 0
 * (the screen as the texture, colour test); the layer is drawn where z is
 * 0xffff.  Then 0xffff again, and the text (Text_TrFlag 1: its dots not 0)
 * or the BG and sprites (2: their dots not 0, the "gd" tiles) are drawn
 * with their own z; the layer is drawn where it is not 0 and z is not
 * 0xffff.
 */
static void ge_prio(const GE_Band *b, const unsigned int *tclut)
{
	const GE_State *s = &b->st;

	if (s->pbefore) {
		GE_TV *v = (GE_TV *)ge_mem(GE_NSTRIP(512) * 2 * sizeof(GE_TV));

		sceGuPixelMask(0xffffffff);
		ge_fill(0, b->y0, s->dotx, b->h, 0, 1);		/* z 0xffff */
		sceGuTexFlush();
		sceGuTexMode(GU_PSM_5650, 0, 0, 0);
		sceGuTexImage(0, 512, 512, 512, GE_VRAM(GE_SCRBUF_L));
		sceGuDisable(GU_ALPHA_TEST);
		sceGuEnable(GU_COLOR_TEST);			/* not 0 */
		sceGuEnable(GU_DEPTH_TEST);
		sceGuDepthFunc(GU_ALWAYS);
		ge_draw(v, ge_quad(v, 0, b->y0, s->dotx, b->h, 0, b->y0, 0, 0, 0));
		sceGuDisable(GU_COLOR_TEST);
		sceGuPixelMask(0);
		sceGuTexFlush();
		sceGuDepthFunc(GU_EQUAL);
		ge_layer_quad(b, 0xffff);			/* where the graphics are 0 */
	}
	if (s->pafter & 4) {
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuEnable(GU_COLOR_TEST);
		ge_layer_quad(b, 0);
		sceGuDisable(GU_COLOR_TEST);
	} else if (s->pafter) {
		sceGuPixelMask(0xffffffff);
		ge_fill(0, b->y0, s->dotx, b->h, 0, 1);		/* z 0xffff */
		ge_zmask = 1;
		if ((s->pafter & 1) && s->ton) {
			sceGuClutLoad(256 / 8, tclut);
			ge_text(b, 0);
		}
		if ((s->pafter & 2) && s->bgon) {
			sceGuClutLoad(256 / 8, tclut);
			if (b->recbuild == ge_buildno) {
				/* the layer pass' draws again */
				const void *tex = NULL;
				int i;

				sceGuTexMode(GU_PSM_T4, 0, 0, 0);
				sceGuEnable(GU_DEPTH_TEST);
				sceGuDepthFunc(GU_ALWAYS);
				for (i = b->rec0; i < b->rec1; i++) {
					const GE_Rec *r = &ge_rec[i];

					if (!r->tex) {
						sceGuDisable(GU_TEXTURE_2D);
						sceGuDisable(GU_ALPHA_TEST);
						sceGuDrawArray(GU_SPRITES, GE_CVFMT, r->n, 0, r->v);
						sceGuEnable(GU_TEXTURE_2D);
					} else {
						if (r->tex != tex) {
							tex = r->tex;
							sceGuTexImage(0, 512, r->th, 512, tex);
						}
						sceGuEnable(GU_ALPHA_TEST);
						sceGuClutMode(GU_PSM_8888, 0, 0x0f, r->blk);
						sceGuDrawArray(GU_SPRITES, GE_TVFMT, r->n, 0, r->v);
					}
					GE_Stat[GE_ST_DRAWS]++;
					GE_Stat[GE_ST_VERTS] += r->n;
				}
			} else
				ge_bg(b, s->mcase, &ge_pal[s->pal]);
		}
		ge_zmask = 0;
		sceGuPixelMask(0);
		sceGuEnable(GU_DEPTH_TEST);
		sceGuDepthFunc(GU_NOTEQUAL);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuEnable(GU_COLOR_TEST);
		ge_layer_quad(b, 0xffff);
		sceGuDisable(GU_COLOR_TEST);
	}
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_ALPHA_TEST);
}

static int ge_layer_band(const GE_State *s)
{
	return s->mode == GE_M || s->mode == GE_GM;
}

static void GE_Render(void *fbp, int passes)
{
	unsigned int *tclut[GE_NPAL], *gclut[GE_NPAL];
	const int dither = sceGuGetStatus(GU_DITHER);
	unsigned t0 = sceKernelGetSystemTimeLow(), t1;
	int i, k;

	if (!ge_nband)
		return;
#define GE_BUILD_TIME(n)	do { t1 = sceKernelGetSystemTimeLow(); \
				     GE_Stat[GE_ST_BUILD_US + (n)] += t1 - t0; t0 = t1; } while (0)

	/* CLUTs: alpha 0 for dot 0 (transparent where drawn so) */
	for (i = 0; i < ge_npal; i++) {
		tclut[i] = (unsigned int *)ge_mem(256 * 4);
		gclut[i] = (unsigned int *)ge_mem(256 * 4);
		for (k = 0; k < 256; k++)
			tclut[i][k] = ge_c32(ge_pal[i].text[k], (k & 15) ? 255 : 0);
		for (k = 0; k < 256; k++)
			gclut[i][k] = ge_c32(ge_pal[i].grp[k], k ? 255 : 0);
	}

	sceGuDisable(GU_DITHER);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_COLOR_TEST);
	sceGuScissor(0, 0, 512, 512);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuAlphaFunc(GU_GREATER, 0, 0xff);
	sceGuColorFunc(GU_NOTEQUAL, 0, 0xffffff);
	sceGuDepthMask(GU_FALSE);	/* depth writes on (only with the test on) */
	sceGuTexFlush();
	GE_BUILD_TIME(0);

	if (passes & GE_P_COPY) {
		for (i = 0; i < ge_nband; i++)
			ge_copy(&ge_band[i]);
		sceGuTexSync();
	}
	GE_BUILD_TIME(1);

	/*
	 * The text/BG layers: per band the fill, then BG and text in the order
	 * of BG_DrawLine/Text_DrawLine (text above BG: BG first).
	 */
	if (passes & (GE_P_FILL | GE_P_BGB | GE_P_TEXT | GE_P_BGA)) {
		sceGuDrawBufferList(GU_PSM_5650, (void *)GE_LAYER, 512);
		for (i = 0; i < ge_nband; i++) {
			const GE_Band *b = &ge_band[i];
			const GE_State *s = &b->st;

			if (!ge_layer_band(s))
				continue;
			ge_dotx = s->dotx;
			ge_lt = sceKernelGetSystemTimeLow();
			if (passes & GE_P_FILL)
				ge_fill(0, b->y0, s->dotx, b->h,
					(s->ton || s->bgon) ? ge_c32(ge_pal[s->pal].text[0], 255) : 0, 1);
			ge_ltick(0);
			if ((passes & GE_P_BGB) && s->mcase && s->bgon) {
				sceGuClutLoad(256 / 8, tclut[s->pal]);
				ge_bg_rec(&ge_band[i], 1);
			}
			ge_lt = sceKernelGetSystemTimeLow();
			if ((passes & GE_P_TEXT) && s->ton) {
				sceGuClutLoad(256 / 8, tclut[s->pal]);
				ge_text(b, !s->mcase);
			}
			ge_ltick(3);
			if ((passes & GE_P_BGA) && !s->mcase && s->bgon) {
				sceGuClutLoad(256 / 8, tclut[s->pal]);
				ge_bg_rec(&ge_band[i], 0);
			}
		}
		sceGuTexSync();
		sceGuTexFlush();
	}
	GE_BUILD_TIME(2);

	/* the screen */
	if (passes & (GE_P_GRP | GE_P_COMP)) {
		sceGuDrawBufferList(GU_PSM_5650, (void *)GE_SCRBUF_L, 512);
		for (i = 0; i < ge_nband; i++) {
			const GE_Band *b = &ge_band[i];
			const GE_State *s = &b->st;

			ge_dotx = s->dotx;
			if ((passes & GE_P_GRP) && (s->mode == GE_G || s->mode == GE_GM))
				s->gm == 2 ? ge_grp16(b) : ge_grp(b, gclut[s->pal]);
			if ((passes & GE_P_COMP) && s->mode == GE_ZERO)
				ge_fill(0, b->y0, s->dotx, b->h, 0, 0);
			if ((passes & GE_P_COMP) && s->prio)
				ge_prio(b, tclut[s->pal]);
			else if ((passes & GE_P_COMP) && ge_layer_band(s)) {
				GE_TV *v = (GE_TV *)ge_mem(GE_NSTRIP(512) * 2 * sizeof(GE_TV));

				sceGuTexMode(GU_PSM_5650, 0, 0, 0);
				sceGuTexImage(0, 512, 256, 512, GE_VRAM(GE_LAYER));
				sceGuDisable(GU_DEPTH_TEST);
				sceGuDisable(GU_ALPHA_TEST);
				if (s->mode == GE_GM)
					sceGuEnable(GU_COLOR_TEST);	/* colour 0 is transparent */
				else
					sceGuDisable(GU_COLOR_TEST);
				ge_draw(v, ge_quad(v, 0, b->y0, s->dotx, b->h, 0, b->y0, 0, 0, 0));
				sceGuDisable(GU_COLOR_TEST);
			}
		}
	}
	GE_BUILD_TIME(3);
#undef GE_BUILD_TIME

	/* back to what WinDraw_Draw expects */
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuScissor(0, 0, 480, 272);
	if (dither)
		sceGuEnable(GU_DITHER);
	sceGuDrawBufferList(GU_PSM_5650, fbp, 512);
	sceGuTexSync();
	sceGuTexFlush();

	if (!(passes & GE_P_END))
		return;
	GE_StatBands += ge_nband;
	for (i = 0; i < ge_nband; i++)
		if (ge_layer_band(&ge_band[i].st) && ge_band[i].st.bgon)
			ge_bgfly = 1;
	ge_nband = 0;
	ge_npal = 0;
	ge_vneed = 0;
	ge_handed();
	ge_inflight = 1;	/* until GE_Done() */
	GE_Guard = 1;
	GE_Stat[GE_ST_RENDERS]++;
}

int GE_Room(void)
{
	return ge_voff + ge_vneed <= GE_VARENA && ge_coff + ge_nband * GE_CBAND + GE_CFIXED <= GE_CLIST;
}

int GE_CanKick(void)
{
	/* (a 65536 colour band may convert GVRAM rows the GE's lists still copy) */
	return !ge_nq && GE_Room() && !(ge_g16pend && ge_g16fly);
}

void *GE_ListMem(int size)
{
	unsigned int *l = ge_clist + ge_coff / 4;

	ge_coff += (size + 63) & ~63;
	return l;
}

void *GE_Build(void *fbp, int passes)
{
	unsigned int *l = ge_clist + ge_coff / 4;

	ge_buildno++;		/* the draws recorded before are not of this list */
	ge_nrec = 0;
	sceGuStart(GU_CALL, l);
	GE_Render(fbp, passes);
	ge_coff += (sceGuFinish() + 63) & ~63;
	if (ge_voff > GE_VARENA || ge_coff > GE_CLIST) {	/* the bounds are wrong */
		static int logged;

		if (!logged++)
			log_printf("GE_Build: list memory overrun, %d/%d %d/%d\n", ge_voff, GE_VARENA, ge_coff, GE_CLIST);
	}
	return l;
}

void GE_LogStats(void)
{
	static const char *const why[] = {
		"gvram", "gvram-mode", "tvram", "fastclear", "rcupdate", "bg", "bgq-full",
		"spr-full", "(unused)", "bands", "pals"
	};
	static const char *const reason[] = {
		"", "debug", "gmode", "trans", "r29", "width", "prio", "twrap", "bgres", "vline"
	};
	const unsigned f = GE_Stat[GE_ST_FRAMES] ? GE_Stat[GE_ST_FRAMES] : 1;
	char buf[512];
	int i, n;

	log_printf("ge %s: %u frames; per frame: lines ge %u cpu %u, bands %u.%02u, lists %u.%02u, draws %u, "
		   "verts %u, pixels %u, build %u us, guard wait %u us, GE %u us (ge time %s)\n",
		   GE_Enabled ? "on" : "off", GE_Stat[GE_ST_FRAMES], GE_StatLines / f, GE_StatCpuLines / f,
		   GE_StatBands / f, GE_StatBands * 100 / f % 100,
		   GE_Stat[GE_ST_RENDERS] / f, GE_Stat[GE_ST_RENDERS] * 100 / f % 100, GE_Stat[GE_ST_DRAWS] / f,
		   GE_Stat[GE_ST_VERTS] / f, GE_Stat[GE_ST_PIXELS] / f, GE_Stat[GE_ST_RENDER_US] / f,
		   GE_Stat[GE_ST_WAIT_US] / f, GE_Stat[GE_ST_GE_US] / f, GE_TimeSync ? "on" : "off");
	log_printf("ge per frame: wait at frame end %u us, cpu-line waits %u.%02u (%u us), bg queue replay %u us; "
		   "build us: setup %u copy %u layer %u (fill %u gd %u spr/bg %u text %u) screen %u dcache %u "
		   "(cpu of this thread %u)\n",
		   GE_Stat[GE_ST_FRAME_WAIT_US] / f, GE_Stat[GE_ST_LINE_WAITS] / f,
		   GE_Stat[GE_ST_LINE_WAITS] * 100 / f % 100, GE_Stat[GE_ST_LINE_WAIT_US] / f,
		   GE_Stat[GE_ST_DONE_US] / f, GE_Stat[GE_ST_BUILD_US] / f, GE_Stat[GE_ST_BUILD_US + 1] / f,
		   GE_Stat[GE_ST_BUILD_US + 2] / f, GE_Stat[GE_ST_LAYER_US] / f, GE_Stat[GE_ST_LAYER_US + 1] / f,
		   GE_Stat[GE_ST_LAYER_US + 2] / f, GE_Stat[GE_ST_LAYER_US + 3] / f,
		   GE_Stat[GE_ST_BUILD_US + 3] / f, GE_Stat[GE_ST_BUILD_US + 4] / f,
		   GE_Stat[GE_ST_BUILD_US + 5] / f);
	log_printf("ge per frame: copied %u bytes, 65536 colour dots converted %u (rows: new %u palette %u "
		   "clear %u written %u, columns %u); pass us (ge time): copy %u "
		   "fill %u bg-below %u text %u bg-above %u grp %u comp %u\n", GE_Stat[GE_ST_COPY_BYTES] / f,
		   GE_Stat[GE_ST_G16_DOTS] / f, GE_Stat[GE_ST_G16_WHY] / f, GE_Stat[GE_ST_G16_WHY + 1] / f,
		   GE_Stat[GE_ST_G16_WHY + 2] / f, GE_Stat[GE_ST_G16_WHY + 3] / f, GE_Stat[GE_ST_G16_WHY + 4] / f,
		   GE_Stat[GE_ST_PASS_US + 0] / f, GE_Stat[GE_ST_PASS_US + 1] / f, GE_Stat[GE_ST_PASS_US + 2] / f,
		   GE_Stat[GE_ST_PASS_US + 3] / f, GE_Stat[GE_ST_PASS_US + 4] / f, GE_Stat[GE_ST_PASS_US + 5] / f,
		   GE_Stat[GE_ST_PASS_US + 6] / f);
	n = snprintf(buf, sizeof(buf), "ge waits (total):");
	for (i = 0; i <= GE_ST_PALS && n < (int)sizeof(buf); i++)
		n += snprintf(buf + n, sizeof(buf) - n, " %s %u", why[i], GE_Stat[i]);
	if (n < (int)sizeof(buf))
		n += snprintf(buf + n, sizeof(buf) - n, "; sprite copies %u, bg scans %u; cpu lines:",
			      GE_Stat[GE_ST_SPR_COPY], GE_Stat[GE_ST_BG_SCAN]);
	for (i = 1; i <= GE_R_VLINE && n < (int)sizeof(buf); i++)
		n += snprintf(buf + n, sizeof(buf) - n, " %s %u", reason[i], GE_Stat[GE_ST_CPU_REASON + i]);
	log_printf("%s\n", buf);
	for (i = 1; i <= GE_R_VLINE; i++) {
		const GE_CpuLine *c = &ge_cpuline[i];

		if (c->frames)
			log_printf("ge cpu lines %s: %u frames, lines %u-%u, last %ux%u R20 %02x VC0 %02x\n",
				   reason[i], c->frames, c->vmin, c->vmax, c->dotx, c->doty, c->r28, c->vc0);
	}
	memset(ge_cpuline, 0, sizeof(ge_cpuline));
	memset(GE_Stat, 0, sizeof(GE_Stat));
	GE_StatLines = GE_StatCpuLines = GE_StatBands = GE_StatFlushes = 0;
}
