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

extern BYTE Debug_Text, Debug_Grp, Debug_Sp;
extern BYTE Sprite_Regs[0x800];
extern BYTE BG[0x8000];

int GE_Enabled = 0;
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
} GE_Band;

#define GE_NBAND	32	/* worst case (8x8 BG, 512 dots, 1-line bands) ~500 KB of the 1 MB display list */
#define GE_NPAL		8

static GE_Band ge_band[GE_NBAND];
static int ge_nband;

typedef struct {
	WORD	text[256];
	WORD	grp[16];
} GE_Pal;

static GE_Pal ge_pal[GE_NPAL];
static int ge_npal, ge_palcur;

/* 0: the CPU must draw this line */
static int ge_eval(GE_State *s)
{
	const int v1 = VCReg1[0], v11 = VCReg1[1], v21 = VCReg2[1];
	int gp, tp, sp, text_on, bg_on, ton, bgon, gon, tdrawed = 0;
	int nops = 0, gfirst = 0, mseen = 0, mfull = 0, bad = 0;

	memset(s, 0, sizeof(*s));
	if (!Debug_Grp || !Debug_Text || !Debug_Sp)
		return 0;
	if ((VCReg0[1] & 7) || (VCReg2[0] & 0x10))	/* 16 colours 512 dots; no translucency */
		return 0;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		return 0;
	if (TextDotX == 0 || TextDotX > 512)
		return 0;
	s->dotx = TextDotX;

	/* graphic pages, as in DrawLine (Grp_DrawLine4) */
	if (v21 & 8)
		s->gpage[s->ng++] = (v11 >> 6) & 3;
	if (v21 & 4)
		s->gpage[s->ng++] = (v11 >> 4) & 3;
	if (v21 & 2)
		s->gpage[s->ng++] = (v11 >> 2) & 3;
	if (v21 & 1)
		s->gpage[s->ng++] = v11 & 3;
	gon = s->ng != 0;

	gp = v1 & 3;
	tp = (v1 >> 2) & 3;
	sp = (v1 >> 4) & 3;
	text_on = (v21 & 0x20) != 0;
	bg_on = (v21 & 0x40) && (BG_Regs[8] & 2) && !(BG_Regs[0x11] & 2);
	s->mcase = !(sp < tp);
	ton = text_on;
	bgon = s->mcase ? 1 : bg_on;	/* text above BG: BG_LineBuf is always filled */

	/* the drawing steps of DrawLine; G: graphics, M(mask): BG_LineBuf */
#define OPG()	do { if (mseen) bad = 1; else if (!nops) gfirst = 1; nops++; } while (0)
#define OPM(m)	do { if (!nops || !(m)) mfull = 1; mseen = 1; nops++; } while (0)
	if ((gp & 2) && gon)
		OPG();
	if ((sp & 2) && bgon) {
		OPM(tdrawed);
		tdrawed = 1;
	}
	if ((tp & 2) && ton) {
		OPM(tdrawed);
		tdrawed = 1;
	}
	if (gp == 1 && gon)
		OPG();
	if (sp == 1 && bgon) {
		OPM(tp == 2);
		tdrawed = 1;
	}
	if (tp == 1 && ton) {
		OPM(sp >= 1);
		tdrawed = 1;
	}
	if (gp == 0 && gon)
		OPG();
	if (sp == 0 && bgon)
		OPM(tp >= 1);
	if (tp == 0 && ton)
		OPM(1);
#undef OPG
#undef OPM
	if (bad || (mseen && !mfull))
		return 0;	/* graphics between text and BG */

	if (!nops)
		s->mode = GE_ZERO;
	else if (gfirst)
		s->mode = mseen ? GE_GM : GE_G;
	else
		s->mode = GE_M;

	if (s->mode == GE_G || s->mode == GE_GM) {
		int p;

		for (p = 0; p < 4; p++) {
			s->gx[p] = GrphScrollX[p] & 0x1ff;
			s->gy[p] = GrphScrollY[p] & 0x1ff;
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
				return 0;	/* the line wraps: odd last dot in Text_DrawLine */
		}
		if (bg_on) {
			const int s1 = ((BG_Regs[0x11] & 4) ? 2 : 1) - ((BG_Regs[0x11] & 16) ? 1 : 0);
			const int s2 = ((CRTC_Regs[0x29] & 4) ? 2 : 1) - ((CRTC_Regs[0x29] & 16) ? 1 : 0);
			DWORD vbg = VLINE;

			if (s1 != s2)
				return 0;	/* VLINEBG not VLINE + constant */
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
	return 1;
}

int GE_Pending(void)
{
	return ge_nband != 0;
}

int GE_Line(void)
{
	GE_State s;
	GE_Band *b;

	if (VLINE >= GE_ROWS || !ge_eval(&s)) {
		GE_StatCpuLines++;
		return 0;
	}
	if (ge_nband == GE_NBAND)
		WinDraw_GESync();	/* draws the bands, empties the palette snapshots */

	if (ge_npal == 0 || GE_PalDirty) {
		GE_PalDirty = 0;
		if (ge_npal == 0 || memcmp(ge_pal[ge_palcur].text, TextPal, sizeof(ge_pal[0].text)) ||
		    memcmp(ge_pal[ge_palcur].grp, GrphPal, sizeof(ge_pal[0].grp))) {
			if (ge_npal == GE_NPAL)
				WinDraw_GESync();
			ge_palcur = ge_npal++;
			memcpy(ge_pal[ge_palcur].text, TextPal, sizeof(ge_pal[0].text));
			memcpy(ge_pal[ge_palcur].grp, GrphPal, sizeof(ge_pal[0].grp));
		}
	}
	s.pal = ge_palcur;

	b = &ge_band[ge_nband - 1];
	if (ge_nband && b->y0 + b->h == (int)VLINE && memcmp(&b->st, &s, sizeof(s)) == 0) {
		b->h++;
	} else {
		b = &ge_band[ge_nband++];
		b->st = s;
		b->y0 = VLINE;
		b->h = 1;
	}
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

/* display list memory, 64-byte aligned */
static void *ge_mem(int size)
{
	unsigned int p = (unsigned int)sceGuGetMemory(size + 64);

	return (void *)((p + 63) & ~63u);
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
	GE_CV *v = (GE_CV *)sceGuGetMemory(2 * sizeof(GE_CV));

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
	sceGuEnable(GU_TEXTURE_2D);
}

/*
 * One textured sprite: screen x, y, w, h from texture u, v (w x h), mirrored
 * when hf / vf, clipped to 0 <= x < ge_dotx.
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

static void ge_draw(GE_TV *start, GE_TV *end)
{
	if (end > start)
		sceGuDrawArray(GU_SPRITES, GE_TVFMT, end - start, 0, start);
}

/*
 * Quads sorted by palette block (16 CLUT offsets): counted in a first walk,
 * written in a second one.
 */
typedef struct {
	int	pass;		/* 0: count, 1: write */
	int	n[16];
	GE_TV	*base[16], *cur[16];
} GE_Batch;

static void ge_batch_init(GE_Batch *bt)
{
	memset(bt, 0, sizeof(*bt));
}

static void ge_batch_alloc(GE_Batch *bt)
{
	int k;

	for (k = 0; k < 16; k++)
		if (bt->n[k])
			bt->base[k] = bt->cur[k] = (GE_TV *)sceGuGetMemory(bt->n[k] * 2 * sizeof(GE_TV));
	bt->pass = 1;
}

static inline void ge_add(GE_Batch *bt, int blk, int x, int y, int w, int h, int u, int v, int hf, int vf, int z)
{
	if (bt->pass == 0)
		bt->n[blk]++;	/* upper bound: before clipping */
	else
		bt->cur[blk] = ge_quad(bt->cur[blk], x, y, w, h, u, v, hf, vf, z);
}

static void ge_batch_draw(GE_Batch *bt)
{
	int k;

	for (k = 0; k < 16; k++)
		if (bt->cur[k] > bt->base[k]) {
			sceGuClutMode(GU_PSM_8888, 0, 0x0f, k);
			ge_draw(bt->base[k], bt->cur[k]);
		}
}

/*
 * The tiles of a BG plane on the band, as bg_drawline_loopx8/16: per line,
 * (TextDotX >> 3 or 4) + 1 tiles from x = -(scroll & (size - 1)).
 * under: the "gd" rectangles (palette block != 0) instead, into cv/ncv.
 */
static void ge_tiles(GE_Batch *bt, const GE_Band *b, int sz, DWORD top, DWORD scx, DWORD scy,
		     DWORD adj, const GE_Pal *pal, GE_CV **cv, int *ncv)
{
	const GE_State *s = &b->st;
	const int sh = (sz == 8) ? 3 : 4;
	const int ncol = (s->dotx >> sh) + 1;
	const DWORD sy = scy + s->lbase + b->y0;	/* scroll + VLINEBG - BG_VLINE */
	const DWORD sx = scx - adj;
	int i = 0;

	while (i < b->h) {
		const DWORD yy = sy + i;
		const int r = yy & (sz - 1);
		const int n = (sz - r < b->h - i) ? sz - r : b->h - i;
		const BYTE *map = BG + top + ((sz == 8) ? ((yy & 0x1f8) << 4) : ((yy & 0x3f0) << 3));
		DWORD col = (sx >> sh) & 63;
		int x = -(int)(sx & (sz - 1));
		int k;

		for (k = 0; k < ncol; k++, x += sz, col = (col + 1) & 63) {
			const DWORD bl = map[col * 2], pat = map[col * 2 + 1];
			const int blk = bl & 15;

			if (cv) {
				if (blk) {
					if (*cv) {
						GE_CV *c = *cv;
						int x0 = x < 0 ? 0 : x, x1 = x + sz > ge_dotx ? ge_dotx : x + sz;
						unsigned int col32 = ge_c32(pal->text[blk * 16], 255);

						c[0].c = c[1].c = col32;
						c[0].x = x0;
						c[1].x = x1;
						c[0].y = b->y0 + i;
						c[1].y = b->y0 + i + n;
						c[0].z = c[1].z = 0;
						c[0].pad = c[1].pad = 0;
						*cv = c + 2;
					} else
						(*ncv)++;
				}
			} else {
				const int pu = (sz == 8) ? (pat & 63) * 8 : (pat & 31) * 16;
				const int pv = (sz == 8) ? (pat >> 6) * 8 : (pat >> 5) * 16;
				const int vf = (bl & 0x80) != 0;

				ge_add(bt, blk, x, b->y0 + i, sz, n, pu, vf ? pv + sz - r - n : pv + r,
				       (bl & 0x40) != 0, vf, 0);
			}
		}
		i += n;
	}
}

/* the sprites of priority 'level' on the band (Sprite_CollectLine) */
static void ge_sprites(GE_Batch *bt, const GE_Band *b, int level)
{
	const WORD *sr = (const WORD *)Sprite_Regs;
	const GE_State *s = &b->st;
	const DWORD l0 = s->lbase + b->y0;	/* VLINEBG - BG_VLINE on the first line */
	int n;

	for (n = 127; n >= 0; n--) {
		const WORD *sp = sr + n * 4;
		DWORD t, ctrl;
		int d, top, i0, i1, pu, pv, vf;

		if ((sp[3] & 3) != level)
			continue;
		t = (sp[0] + s->hadj) & 0x3ff;
		if (t >= (DWORD)s->dotx + 16)
			continue;
		/* line i of the band shows row i - top, if 0-15 */
		d = (int)((DWORD)(sp[1] & 0x3ff) - l0 - 1);
		top = d - 15;
		i0 = top < 0 ? 0 : top;
		i1 = top + 16 > b->h ? b->h : top + 16;
		if (i0 >= i1)
			continue;
		ctrl = sp[2];
		pu = (ctrl & 31) * 16;
		pv = ((ctrl >> 5) & 7) * 16;
		vf = (ctrl & 0x8000) != 0;
		ge_add(bt, (ctrl >> 8) & 15, (int)t - 16, b->y0 + i0, 16, i1 - i0, pu,
		       vf ? pv + 16 - (i0 - top) - (i1 - i0) : pv + (i0 - top),
		       (ctrl & 0x4000) != 0, vf, n * 256);
	}
}

static void ge_bgplane(const GE_Band *b, int sz, DWORD top, DWORD scx, DWORD scy, DWORD adj)
{
	GE_Batch bt;

	ge_batch_init(&bt);
	ge_tiles(&bt, b, sz, top, scx, scy, adj, NULL, NULL, NULL);
	ge_batch_alloc(&bt);
	ge_tiles(&bt, b, sz, top, scx, scy, adj, NULL, NULL, NULL);
	if (sz == 8)
		sceGuTexImage(0, 512, 32, 512, GE_VRAM(GE_ATLAS8));
	else
		sceGuTexImage(0, 512, 128, 512, GE_VRAM(GE_ATLAS16));
	sceGuDisable(GU_DEPTH_TEST);
	ge_batch_draw(&bt);
}

static void ge_spritelevel(const GE_Band *b, int level)
{
	GE_Batch bt;

	ge_batch_init(&bt);
	ge_sprites(&bt, b, level);
	ge_batch_alloc(&bt);
	ge_sprites(&bt, b, level);
	sceGuTexImage(0, 512, 128, 512, GE_VRAM(GE_ATLAS16));
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthFunc(GU_LEQUAL);
	ge_batch_draw(&bt);
}

static void ge_under(const GE_Band *b, int sz, DWORD top, DWORD scx, DWORD scy, DWORD adj, const GE_Pal *pal)
{
	GE_CV *cv = NULL, *base;
	int n = 0;

	ge_tiles(NULL, b, sz, top, scx, scy, adj, pal, &cv, &n);
	if (!n)
		return;
	base = cv = (GE_CV *)sceGuGetMemory(n * 2 * sizeof(GE_CV));
	ge_tiles(NULL, b, sz, top, scx, scy, adj, pal, &cv, &n);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDrawArray(GU_SPRITES, GE_CVFMT, cv - base, 0, base);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_ALPHA_TEST);
}

/* BG_DrawLine without the fill: sprites and BG planes */
static void ge_bg(const GE_Band *b, int gd, const GE_Pal *pal)
{
	const GE_State *s = &b->st;
	const int bg1 = s->chr8 && (s->bg9 & 8), bg0 = s->bg9 & 1;
	const int sz0 = s->chr8 ? 8 : 16;
	/* the original passed no H adjust for 16x16 without graphics */
	const DWORD adj0 = (s->chr8 || gd) ? s->hadj : 0;

	if (gd) {
		/* under everything: BG0's, then BG1's (BG1 comes first, wins) */
		if (bg0)
			ge_under(b, sz0, s->bg0top, s->bg0sx, s->bg0sy, adj0, pal);
		if (bg1)
			ge_under(b, 8, s->bg1top, s->bg1sx, s->bg1sy, s->hadj, pal);
	}
	sceGuTexMode(GU_PSM_T4, 0, 0, 0);
	sceGuEnable(GU_ALPHA_TEST);
	ge_spritelevel(b, 1);
	if (bg1)
		ge_bgplane(b, 8, s->bg1top, s->bg1sx, s->bg1sy, s->hadj);
	ge_spritelevel(b, 2);
	if (bg0)
		ge_bgplane(b, sz0, s->bg0top, s->bg0sx, s->bg0sy, adj0);
	ge_spritelevel(b, 3);
}

/* Text_DrawLine: TextDrawWork, 1 byte per dot, as a T8 texture */
static void ge_text(const GE_Band *b, int opaque)
{
	const GE_State *s = &b->st;
	int i = 0;

	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
	sceGuDisable(GU_DEPTH_TEST);
	if (opaque)
		sceGuDisable(GU_ALPHA_TEST);
	else
		sceGuEnable(GU_ALPHA_TEST);
	while (i < b->h) {
		const int row = (s->ty + b->y0 + i) & 1023;
		const int n = (1024 - row < b->h - i) ? 1024 - row : b->h - i;
		int c0;

		for (c0 = 0; c0 < s->dotx; c0 += 256) {
			const int x = s->tx + c0;
			const int w = (s->dotx - c0 < 256) ? s->dotx - c0 : 256;
			GE_TV *v = (GE_TV *)sceGuGetMemory(2 * sizeof(GE_TV));

			sceGuTexImage(0, 512, 512, 1024, TextDrawWork + row * 1024 + (x & ~15));
			ge_draw(v, ge_quad(v, c0, b->y0 + i, w, n, x & 15, 0, 0, 0, 0));
		}
		i += n;
	}
}

/* the text/BG layer of a band, into the layer buffer */
static void ge_layer(const GE_Band *b, const unsigned int *tclut)
{
	const GE_State *s = &b->st;
	const GE_Pal *pal = &ge_pal[s->pal];

	ge_fill(0, b->y0, s->dotx, b->h, (s->ton || s->bgon) ? ge_c32(pal->text[0], 255) : 0, 1);
	sceGuClutLoad(256 / 8, tclut);
	if (!s->mcase) {
		if (s->ton)
			ge_text(b, 1);
		if (s->bgon)
			ge_bg(b, 0, pal);
	} else {
		if (s->bgon)
			ge_bg(b, 1, pal);
		if (s->ton)
			ge_text(b, 0);
	}
}

/* the graphic pages of a band (Grp_DrawLine4), into the screen texture */
static void ge_grp(const GE_Band *b, const unsigned int *gclut)
{
	const GE_State *s = &b->st;
	int k;

	sceGuClutLoad(16 / 8, gclut);
	sceGuTexMode(GU_PSM_T16, 0, 0, 0);
	sceGuDisable(GU_DEPTH_TEST);
	for (k = 0; k < s->ng; k++) {
		const int p = s->gpage[k];
		const int x = s->gx[p];
		/*
		 * Grp_DrawLine4: dots x .. 510 of the line, then the dot at 511 of
		 * the line above (GVRAM word row * 512 - 1), then 0, 1, ...
		 */
		int n1 = 511 - x;
		GE_TV *v, *e;
		int i;

		if (n1 > s->dotx)
			n1 = s->dotx;
		sceGuClutMode(GU_PSM_8888, p * 4, 0x0f, 0);
		if (k == 0)
			sceGuDisable(GU_ALPHA_TEST);
		else
			sceGuEnable(GU_ALPHA_TEST);

		v = e = (GE_TV *)sceGuGetMemory(4 * 2 * sizeof(GE_TV));
		for (i = 0; i < b->h; ) {
			const int row = (s->gy[p] + b->y0 + i) & 511;
			const int n = (512 - row < b->h - i) ? 512 - row : b->h - i;

			e = ge_quad(e, 0, b->y0 + i, n1, n, x, row, 0, 0, 0);
			if (n1 + 1 < s->dotx)
				e = ge_quad(e, n1 + 1, b->y0 + i, s->dotx - n1 - 1, n, 0, row, 0, 0, 0);
			i += n;
		}
		sceGuTexImage(0, 512, 512, 512, GVRAM);
		ge_draw(v, e);

		if (n1 < s->dotx) {
			/* the odd dot: GVRAM - 1 line as the texture, so that row 0 reads GVRAM[-2] too */
			v = e = (GE_TV *)sceGuGetMemory(2 * 2 * sizeof(GE_TV));
			for (i = 0; i < b->h; ) {
				const int row = (s->gy[p] + b->y0 + i) & 511;
				const int n = (512 - row < b->h - i) ? 512 - row : b->h - i;

				e = ge_quad(e, n1, b->y0 + i, 1, n, 511, row, 0, 0, 0);
				i += n;
			}
			sceGuTexImage(0, 512, 512, 512, (const void *)((unsigned int)GVRAM - 1024));
			ge_draw(v, e);
		}
	}
}

void GE_Render(void *fbp)
{
	unsigned int *tclut[GE_NPAL], *gclut[GE_NPAL];
	const int dither = sceGuGetStatus(GU_DITHER);
	int i, k;

	if (!ge_nband)
		return;
	GE_StatBands += ge_nband;

	/* CLUTs: alpha 0 for dot 0 (transparent where drawn so) */
	for (i = 0; i < ge_npal; i++) {
		tclut[i] = (unsigned int *)ge_mem(256 * 4);
		gclut[i] = (unsigned int *)ge_mem(16 * 4);
		for (k = 0; k < 256; k++)
			tclut[i][k] = ge_c32(ge_pal[i].text[k], (k & 15) ? 255 : 0);
		for (k = 0; k < 16; k++)
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

	/* the text/BG layers */
	sceGuDrawBufferList(GU_PSM_5650, (void *)GE_LAYER, 512);
	for (i = 0; i < ge_nband; i++) {
		const GE_Band *b = &ge_band[i];

		ge_dotx = b->st.dotx;
		if (b->st.mode == GE_M || b->st.mode == GE_GM)
			ge_layer(b, tclut[b->st.pal]);
	}
	sceGuTexSync();
	sceGuTexFlush();

	/* the screen */
	sceGuDrawBufferList(GU_PSM_5650, (void *)GE_SCRBUF_L, 512);
	for (i = 0; i < ge_nband; i++) {
		const GE_Band *b = &ge_band[i];
		const GE_State *s = &b->st;

		ge_dotx = s->dotx;
		switch (s->mode) {
		case GE_ZERO:
			ge_fill(0, b->y0, s->dotx, b->h, 0, 0);
			break;
		case GE_G:
		case GE_GM:
			ge_grp(b, gclut[s->pal]);
			if (s->mode == GE_G)
				break;
			/* FALLTHROUGH */
		case GE_M: {
			GE_TV *v = (GE_TV *)sceGuGetMemory(2 * sizeof(GE_TV));

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
			break;
		}
		}
	}

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

	ge_nband = 0;
	ge_npal = 0;
}
