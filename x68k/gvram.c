// ---------------------------------------------------------------------------------------
//  GVRAM.C - Graphic VRAM
// ---------------------------------------------------------------------------------------

#include	"common.h"
#include	"windraw.h"
#include	"winx68k.h"
#include	"crtc.h"
#include	"palette.h"
#include	"tvram.h"
#include	"gvram.h"
#include	"m68000.h"
#include	"memory.h"

#if defined(__GNUC__)
#define GRP_ALIGN	__attribute__((__aligned__(4)))
#else
#define GRP_ALIGN
#endif

	BYTE	GVRAM[0x80000] GRP_ALIGN;
	WORD	Grp_LineBuf[1024] GRP_ALIGN;
	WORD	Grp_LineBufSP[1024] GRP_ALIGN;		// 特殊プライオリティ／半透明用バッファ
	WORD	Grp_LineBufSP2[1024] GRP_ALIGN;		// 半透明ベースプレーン用バッファ（非半透明ビット格納）

	WORD	Pal16Adr[256];			// 16bit color パレットアドレス計算用

// xxx: for little endian only
#define GET_WORD_W8(src) (*(BYTE *)(src) | *((BYTE *)(src) + 1) << 8)

#if !defined(USE_ASM) && !(defined(USE_GAS) && defined(__i386__))
// -----------------------------------------------------------------------
//   Portable C line decoders (used by every build without the x86 asm)
//
//   These produce exactly the same line buffers as the straightforward
//   per-pixel loops they replace, including their quirks (where a line
//   wraps at the 512 dot boundary, which reads go outside the line, ...),
//   but split each line at its wrap points instead of testing every dot,
//   read GVRAM with aligned 16-bit loads instead of assembling bytes, and
//   write two dots at a time where that is possible.
// -----------------------------------------------------------------------
#if defined(__GNUC__)
typedef WORD  __attribute__((__may_alias__)) GWORD;
typedef DWORD __attribute__((__may_alias__)) GDWORD;
#define GRP_INLINE	static inline __attribute__((__always_inline__))
#else
typedef WORD  GWORD;
typedef DWORD GDWORD;
#define GRP_INLINE	static __inline
#endif

// two dots for one 32-bit store into a WORD line buffer
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define GRP_PAIR(a, b)	(((DWORD)(a) << 16) | (DWORD)(b))
#else
#define GRP_PAIR(a, b)	((DWORD)(a) | ((DWORD)(b) << 16))
#endif

#define GRP_ODD(p)	(((size_t)(p)) & 2)

// GVRAM offset of the first byte of the line shown at VLINE
GRP_INLINE DWORD Grp_LineOfs(DWORD scry)
{
	DWORD y = scry + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	return (y & 0x1ff) << 10;
}

// translucency: mix a dot with the dot under it (Grp_LineBufSP)
GRP_INLINE DWORD Grp_Blend(DWORD v0, DWORD v)
{
	v0 &= Pal_HalfMask;
	if (v & Ibit)
		v0 |= Pal_Ix2;
	v &= Pal_HalfMask;
	return (v + v0) >> 1;
}

// ---- 16 colours: one page is the nibble at bit 'sh' of each word ----
//
// Two dots are looked up at once in a table indexed by their two nibbles
// (low dot in bits 0-3, high dot in bits 4-7).  The tables are made from
// GrphPal[0..15] and remade whenever those change.

typedef struct {
	DWORD	col;		// both dots, 0 where the nibble is 0
	DWORD	keep;		// 0xffff where the nibble is 0 (keep what is there)
} GRP4TR;

static DWORD	Grp4_OpTab[256] GRP_ALIGN;
static GRP4TR	Grp4_TrTab[256] GRP_ALIGN;
static WORD	Grp4_PalSnap[16];
static int	Grp4_TabOk = 0;		// bit0: Grp4_OpTab, bit1: Grp4_TrTab

static void Grp4_MakeTab(int which)
{
	DWORD i, lo, hi;

	if (which & 1) {
		for (i = 0; i < 256; i++)
			Grp4_OpTab[i] = GRP_PAIR(Grp4_PalSnap[i & 15], Grp4_PalSnap[i >> 4]);
	}
	if (which & 2) {
		for (i = 0; i < 256; i++) {
			lo = i & 15;
			hi = i >> 4;
			Grp4_TrTab[i].col = GRP_PAIR(lo ? Grp4_PalSnap[lo] : 0, hi ? Grp4_PalSnap[hi] : 0);
			Grp4_TrTab[i].keep = GRP_PAIR(lo ? 0 : 0xffff, hi ? 0 : 0xffff);
		}
	}
	Grp4_TabOk |= which;
}

// make sure the table 'which' (1: opaque, 2: transparent) is up to date
GRP_INLINE void Grp4_PalCheck(int which)
{
	DWORD i, diff;

#define GRP4_D(i)	((GrphPal[i] ^ Grp4_PalSnap[i]) | (GrphPal[i+1] ^ Grp4_PalSnap[i+1]))
	diff = GRP4_D(0) | GRP4_D(2) | GRP4_D(4) | GRP4_D(6)
	     | GRP4_D(8) | GRP4_D(10) | GRP4_D(12) | GRP4_D(14);
#undef GRP4_D
	if (diff) {
		for (i = 0; i < 16; i++)
			Grp4_PalSnap[i] = GrphPal[i];
		Grp4_TabOk = 0;
	}
	if (!(Grp4_TabOk & which))
		Grp4_MakeTab(which);
}

#define GRP4_IDX(a, b, sh)	((((a) >> (sh)) & 15) | ((((b) >> (sh)) & 15) << 4))

GRP_INLINE void Grp4_Opaq(const GWORD *src, WORD *dst, DWORD n, const int sh)
{
	const DWORD *tab = Grp4_OpTab;

	if (n == 0)
		return;
	if (GRP_ODD(dst)) {
		*dst++ = GrphPal[(*src++ >> sh) & 15];
		n--;
	}
	for (; n >= 4; n -= 4) {
		DWORD a = src[0], b = src[1], c = src[2], d = src[3];
		src += 4;
		((GDWORD *)dst)[0] = tab[GRP4_IDX(a, b, sh)];
		((GDWORD *)dst)[1] = tab[GRP4_IDX(c, d, sh)];
		dst += 4;
	}
	for (; n; n--)
		*dst++ = GrphPal[(*src++ >> sh) & 15];
}

#if !defined(PSP) && !defined(GRP4_OPAQ_TABLE)
/*
 * One dot at a time, as before b3bc8eb: on an x86 host faster than the
 * pair table of Grp4_Opaq (tools/bench: screens with one opaque 16-colour
 * page 18-32%); the PSP build (or GRP4_OPAQ_TABLE) uses the table.
 */
GRP_INLINE void Grp4_OpaqDots(const GWORD *src, WORD *dst, DWORD n, const int sh)
{
	for (; n; n--)
		*dst++ = GrphPal[(*src++ >> sh) & 15];
}
#define GRP4_OPAQ	Grp4_OpaqDots
#define GRP4_OPAQ_TAB	0
#else
#define GRP4_OPAQ	Grp4_Opaq
#define GRP4_OPAQ_TAB	1
#endif

GRP_INLINE void Grp4_Trans(const GWORD *src, WORD *dst, DWORD n, const int sh)
{
	const GRP4TR *tab = Grp4_TrTab;
	DWORD a;

	if (n == 0)
		return;
	if (GRP_ODD(dst)) {
		a = (*src++ >> sh) & 15;
		if (a)
			*dst = GrphPal[a];
		dst++;
		n--;
	}
	for (; n >= 2; n -= 2) {
		a = GRP4_IDX(src[0], src[1], sh);
		src += 2;
		if (a) {
			const GRP4TR *t = &tab[a];
			*(GDWORD *)dst = (*(GDWORD *)dst & t->keep) | t->col;
		}
		dst += 2;
	}
	if (n) {
		a = (*src >> sh) & 15;
		if (a)
			*dst = GrphPal[a];
	}
}

// the second run of a line starts 0x200 words before where the first ended
#define GRP4_RUNS(func, sh) do {					\
		func(src, Grp_LineBuf, n1, sh);				\
		func(src + n1 - 0x200, Grp_LineBuf + n1, n - n1, sh);	\
	} while (0)

static void Grp_DrawLine4_C(DWORD page, int opaq)
{
	DWORD x, n1, n = TextDotX;
	const GWORD *src;

	page &= 3;
	x = GrphScrollX[page] & 0x1ff;
	src = (const GWORD *)(GVRAM + Grp_LineOfs(GrphScrollY[page]) + x * 2);

	// NB: the first run is one dot short of the end of the line (so the
	// last dot of the line is read from the line above); kept as it was.
	n1 = x ^ 0x1ff;
	if (n1 >= n)
		n1 = n;

	if (opaq) {
		if (GRP4_OPAQ_TAB)
			Grp4_PalCheck(1);
		switch (page) {
		case 0: GRP4_RUNS(GRP4_OPAQ, 0); break;
		case 1: GRP4_RUNS(GRP4_OPAQ, 4); break;
		case 2: GRP4_RUNS(GRP4_OPAQ, 8); break;
		default: GRP4_RUNS(GRP4_OPAQ, 12); break;
		}
	} else {
		Grp4_PalCheck(2);
		switch (page) {
		case 0: GRP4_RUNS(Grp4_Trans, 0); break;
		case 1: GRP4_RUNS(Grp4_Trans, 4); break;
		case 2: GRP4_RUNS(Grp4_Trans, 8); break;
		default: GRP4_RUNS(Grp4_Trans, 12); break;
		}
	}
}

// Several 16 colour pages in one pass (see Grp_DrawLine4Multi).
//
// The k-th page drawn (k = 0 for the opaque one) wins over the ones before
// it where its nibble is not 0.  Grp4_MRank[0][b] gives, for the low byte b
// of a GVRAM word (pages 0 and 1), (k + 1) << 4 | nibble of the latest
// such page with a non-0 nibble, or 0; Grp4_MRank[1] the same for the high
// byte (pages 2 and 3).  The larger of the two, & 15, is the colour index.

static BYTE	Grp4_MRank[2][256];
static DWORD	Grp4_MKey = 0xffffffff;

static void Grp4_MakeRank(DWORD pages, int n)
{
	int k, h, b, best, rank[4] = { 0, 0, 0, 0 };

	for (k = 0; k < n; k++)
		rank[(pages >> (k * 2)) & 3] = k + 1;	// a page drawn twice: latest
	for (h = 0; h < 2; h++) {
		for (b = 0; b < 256; b++) {
			int lo = b & 15, hi = b >> 4;
			int rlo = rank[h * 2], rhi = rank[h * 2 + 1];

			best = 0;
			if (rlo && lo)
				best = (rlo << 4) | lo;
			if (rhi && hi && ((rhi << 4) | hi) > best)
				best = (rhi << 4) | hi;
			Grp4_MRank[h][b] = best;
		}
	}
}

GRP_INLINE DWORD Grp4_Pick(DWORD w)
{
	DWORD l = Grp4_MRank[0][w & 0xff], h = Grp4_MRank[1][w >> 8];

	return ((l > h) ? l : h) & 15;
}

static void Grp4_MultiRun(const GWORD *src, WORD *dst, DWORD n)
{
	const DWORD *tab = Grp4_OpTab;
	DWORD a, b;

	if (n == 0)
		return;
	if (GRP_ODD(dst)) {
		*dst++ = GrphPal[Grp4_Pick(*src++)];
		n--;
	}
	for (; n >= 2; n -= 2) {
		a = Grp4_Pick(src[0]);
		b = Grp4_Pick(src[1]);
		src += 2;
		*(GDWORD *)dst = tab[a | (b << 4)];
		dst += 2;
	}
	if (n)
		*dst = GrphPal[Grp4_Pick(*src)];
}

// Pages drawn in descending page order (VCReg1 = e4, the usual priority),
// each at its own scroll position.  Masked to their own nibble and ORed,
// the words of the pages make a word whose lowest non-0 nibble is the dot
// (the last page drawn that is not 0 there); 0 if none.

static BYTE	Grp4_Lnz[256];		// lowest non-0 nibble of a byte, or 0
static int	Grp4_LnzOk = 0;

#if defined(__GNUC__)
typedef struct { DWORD v; } __attribute__((__packed__, __may_alias__)) GU32;
GRP_INLINE DWORD Grp_Ld32(const GWORD *p)	// two dots, any 2-byte alignment
{
	return ((const GU32 *)p)->v;
}
#else
GRP_INLINE DWORD Grp_Ld32(const GWORD *p)
{
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}
#endif

// l if it is not 0, else h
GRP_INLINE DWORD Grp_Sel(DWORD l, DWORD h)
{
#if defined(__mips__)
	__asm__("movz %0, %1, %0" : "+r"(l) : "r"(h));
	return l;
#else
	return l ? l : h;
#endif
}

GRP_INLINE DWORD Grp4_LnzW(DWORD w)
{
	return Grp_Sel(Grp4_Lnz[w & 0xff], Grp4_Lnz[w >> 8]);
}

// po: the opaque (highest) page, its nibble at bit 'sho'; q1..q3: the
// others with their nibble masks k1..k3 (mask 0 for a page not drawn)
#define GRP4_W(i)	((po[i] & mo) | (q1[i] & k1) | (q2[i] & k2) | (q3[i] & k3))

GRP_INLINE void Grp4_DescRun(const GWORD *po, const GWORD *q1, const GWORD *q2,
			     const GWORD *q3, DWORD k1, DWORD k2, DWORD k3,
			     WORD *dst, DWORD n, const int sho)
{
	const DWORD *tab = Grp4_OpTab;
	const BYTE *lnz = Grp4_Lnz;
	const DWORD mo = 15 << sho, MO = mo | (mo << 16);
	const DWORD K1 = k1 | (k1 << 16), K2 = k2 | (k2 << 16), K3 = k3 | (k3 << 16);
	DWORD a, b, c, d, A, B, a1, b1, a2, b2, a3, b3;

	if (n && GRP_ODD(dst)) {
		*dst++ = GrphPal[Grp4_LnzW(GRP4_W(0))];
		po++; q1++; q2++; q3++;
		n--;
	}
	for (; n >= 4; n -= 4) {
		a1 = Grp_Ld32(q1); b1 = Grp_Ld32(q1 + 2);
		a2 = Grp_Ld32(q2); b2 = Grp_Ld32(q2 + 2);
		a3 = Grp_Ld32(q3); b3 = Grp_Ld32(q3 + 2);
		if ((((a1 | b1) & K1) | ((a2 | b2) & K2) | ((a3 | b3) & K3)) == 0) {
			// only the opaque page shows
			a = po[0]; b = po[1]; c = po[2]; d = po[3];
			((GDWORD *)dst)[0] = tab[GRP4_IDX(a, b, sho)];
			((GDWORD *)dst)[1] = tab[GRP4_IDX(c, d, sho)];
		} else {
			// two dots per word: the pages' nibbles ORed in place
			A = (Grp_Ld32(po) & MO) | (a1 & K1) | (a2 & K2) | (a3 & K3);
			B = (Grp_Ld32(po + 2) & MO) | (b1 & K1) | (b2 & K2) | (b3 & K3);
			a = Grp_Sel(lnz[A & 0xff], lnz[(A >> 8) & 0xff]);
			b = Grp_Sel(lnz[(A >> 16) & 0xff], lnz[A >> 24]);
			c = Grp_Sel(lnz[B & 0xff], lnz[(B >> 8) & 0xff]);
			d = Grp_Sel(lnz[(B >> 16) & 0xff], lnz[B >> 24]);
			((GDWORD *)dst)[0] = tab[a | (b << 4)];
			((GDWORD *)dst)[1] = tab[c | (d << 4)];
		}
		po += 4; q1 += 4; q2 += 4; q3 += 4;
		dst += 4;
	}
	for (; n; n--) {
		*dst++ = GrphPal[Grp4_LnzW(GRP4_W(0))];
		po++; q1++; q2++; q3++;
	}
}
#undef GRP4_W

// pages: the pages drawn, highest first (pg[0] opaque), n of them, n >= 2
static void Grp4_Desc(const DWORD *pg, int n)
{
	const GWORD *p[4];
	DWORD wrap[4], i, end, cnt = TextDotX, x, k1, k2, k3;
	int k;

	if (!Grp4_LnzOk) {
		for (k = 0; k < 256; k++)
			Grp4_Lnz[k] = (k & 15) ? (k & 15) : (k >> 4);
		Grp4_LnzOk = 1;
	}
	Grp4_PalCheck(1);

	for (k = 0; k < n; k++) {
		x = GrphScrollX[pg[k]] & 0x1ff;
		p[k] = (const GWORD *)(GVRAM + Grp_LineOfs(GrphScrollY[pg[k]]) + x * 2);
		// as Grp_DrawLine4_C: back 0x200 words after x ^ 0x1ff dots
		wrap[k] = x ^ 0x1ff;
		if (wrap[k] >= cnt)
			wrap[k] = cnt;
	}
	for (; k < 4; k++) {			// not drawn: read the opaque page, masked out
		p[k] = p[0];
		wrap[k] = wrap[0];
	}
	k1 = (n > 1) ? 15 << (pg[1] * 4) : 0;
	k2 = (n > 2) ? 15 << (pg[2] * 4) : 0;
	k3 = (n > 3) ? 15 << (pg[3] * 4) : 0;

	for (i = 0; i < cnt; i = end) {
		end = cnt;
		for (k = 0; k < 4; k++) {
			if (wrap[k] == i)
				p[k] -= 0x200;
			if (wrap[k] > i && wrap[k] < end)
				end = wrap[k];
		}
		switch (pg[0]) {
		case 1: Grp4_DescRun(p[0], p[1], p[2], p[3], k1, k2, k3, Grp_LineBuf + i, end - i, 4); break;
		case 2: Grp4_DescRun(p[0], p[1], p[2], p[3], k1, k2, k3, Grp_LineBuf + i, end - i, 8); break;
		default: Grp4_DescRun(p[0], p[1], p[2], p[3], k1, k2, k3, Grp_LineBuf + i, end - i, 12); break;
		}
		for (k = 0; k < 4; k++)
			p[k] += end - i;
	}
}

// -----------------------------------------------------------------------
//   Grp_DrawLine4Multi(pages, n): the n (1..4) 16 colour pages packed two
//   bits each in 'pages' (first in bits 0-1), the first opaque and each
//   following one over it.  Exactly the same as
//	Grp_DrawLine4(pages & 3, 1);
//	Grp_DrawLine4((pages >> 2) & 3, 0); ...
//   but in one pass when the pages are drawn in descending page order
//   (each at its own scroll position) or are all at the same position.
// -----------------------------------------------------------------------
void FASTCALL Grp_DrawLine4Multi(DWORD pages, int n)
{
	DWORD p0 = pages & 3, x, y, n1, cnt = TextDotX;
	int k;
	const GWORD *src;

	if (n <= 0)
		return;
	if (n > 4)
		n = 4;
	if (n >= 2) {
		DWORD pg[4];
		for (k = 0; k < n; k++)
			pg[k] = (pages >> (k * 2)) & 3;
		for (k = 1; k < n; k++)
			if (pg[k] >= pg[k - 1])
				break;
		if (k == n) {			// strictly descending: any scroll
			Grp4_Desc(pg, n);
			return;
		}
	}
	x = GrphScrollX[p0] & 0x1ff;
	y = Grp_LineOfs(GrphScrollY[p0]);
	for (k = 1; k < n; k++) {
		DWORD p = (pages >> (k * 2)) & 3;
		if ((GrphScrollX[p] & 0x1ff) != x || Grp_LineOfs(GrphScrollY[p]) != y)
			break;
	}
	if (n == 1 || k < n) {			// not all at the same place
		Grp_DrawLine4_C(p0, 1);
		for (k = 1; k < n; k++)
			Grp_DrawLine4_C((pages >> (k * 2)) & 3, 0);
		return;
	}
	pages &= (1 << (n * 2)) - 1;
	if (Grp4_MKey != (pages | (n << 8))) {
		Grp4_MakeRank(pages, n);
		Grp4_MKey = pages | (n << 8);
	}

	Grp4_PalCheck(1);
	src = (const GWORD *)(GVRAM + y + x * 2);
	n1 = x ^ 0x1ff;				// as Grp_DrawLine4_C
	if (n1 >= cnt)
		n1 = cnt;
	Grp4_MultiRun(src, Grp_LineBuf, n1);
	Grp4_MultiRun(src + n1 - 0x200, Grp_LineBuf + n1, cnt - n1);
}

// 1024 dot mode: the right half of the line is the next nibble
static void Grp_DrawLine4h_C(void)
{
	const GWORD *src;
	const WORD *pal = GrphPal;
	WORD *dst = Grp_LineBuf;
	DWORD x, y, run, n = TextDotX;
	int bits;

	y = GrphScrollY[0] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y &= 0x3ff;

	if ((y & 0x200) == 0x000) {
		y <<= 10;
		bits = (GrphScrollX[0] & 0x200) ? 4 : 0;
	} else {
		y = (y & 0x1ff) << 10;
		bits = (GrphScrollX[0] & 0x200) ? 12 : 8;
	}

	x = GrphScrollX[0] & 0x1ff;
	src = (const GWORD *)(GVRAM + y + x * 2);
	run = 512 - x;

	while (n) {
		if (run > n)
			run = n;
		n -= run;
		for (; run; run--)
			*dst++ = pal[(*src++ >> bits) & 0x0f];
		src -= 0x200;
		bits ^= 4;
		run = 512;
	}
}

// ---- base page for translucency / special priority ----

GRP_INLINE void Grp4_SP(const GWORD *src, WORD *sp, WORD *sp2, DWORD n, const int sh)
{
	const WORD *pal = GrphPal;
	DWORD a, b, ca, cb, ma, mb;

	if (n == 0)
		return;
	if (GRP_ODD(sp)) {
		a = (*src++ >> sh) & 15;
		ca = pal[a & 0x0e];
		ma = 0 - (a & 1);
		*sp++ = ca & ma;
		*sp2++ = ca & ~ma;
		n--;
	}
	for (; n >= 2; n -= 2) {
		a = (src[0] >> sh) & 15;
		b = (src[1] >> sh) & 15;
		src += 2;
		ca = pal[a & 0x0e];
		cb = pal[b & 0x0e];
		ma = 0 - (a & 1);
		mb = 0 - (b & 1);
		*(GDWORD *)sp = GRP_PAIR(ca & ma, cb & mb);
		*(GDWORD *)sp2 = GRP_PAIR(ca & ~ma, cb & ~mb);
		sp += 2;
		sp2 += 2;
	}
	if (n) {
		a = (*src >> sh) & 15;
		ca = pal[a & 0x0e];
		ma = 0 - (a & 1);
		*sp = ca & ma;
		*sp2 = ca & ~ma;
	}
}

#define GRP4SP_RUNS(sh) do {							\
		Grp4_SP(src, Grp_LineBufSP, Grp_LineBufSP2, n1, sh);		\
		Grp4_SP(src + n1 - 0x200, Grp_LineBufSP + n1,			\
			Grp_LineBufSP2 + n1, n - n1, sh);			\
	} while (0)

static void Grp_DrawLine4SP_C(DWORD page, DWORD scrx, DWORD scry)
{
	DWORD x, n1, n = TextDotX;
	const GWORD *src;

	x = scrx & 0x1ff;
	src = (const GWORD *)(GVRAM + Grp_LineOfs(scry) + x * 2);
	n1 = 512 - x;
	if (n1 >= n)
		n1 = n;

	switch (page & 3) {
	case 0: GRP4SP_RUNS(0); break;
	case 1: GRP4SP_RUNS(4); break;
	case 2: GRP4SP_RUNS(8); break;
	default: GRP4SP_RUNS(12); break;
	}
}

static void Grp_DrawLine4hSP_C(void)
{
	const GWORD *src;
	const WORD *pal = GrphPal;
	DWORD x, y, i, n1, n = TextDotX;
	DWORD v, c, m;
	int bits;

	y = GrphScrollY[0] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y &= 0x3ff;

	if ((y & 0x200) == 0x000) {
		y <<= 10;
		bits = (GrphScrollX[0] & 0x200) ? 4 : 0;
	} else {
		y = (y & 0x1ff) << 10;
		bits = (GrphScrollX[0] & 0x200) ? 12 : 8;
	}

	x = GrphScrollX[0] & 0x1ff;
	src = (const GWORD *)(GVRAM + y + x * 2);
	n1 = 512 - x;
	if (n1 >= n)
		n1 = n;

	// NB: after the first run the source goes back 0x400 words (two
	// lines), as it always did.
	for (i = 0; i < n1; i++) {
		v = *src++ >> bits;
		c = pal[v & 0x0e];
		m = 0 - (v & 1);
		Grp_LineBufSP[i] = c & m;
		Grp_LineBufSP2[i] = c & ~m;
	}
	src -= 0x400;
	for (; i < n; i++) {
		v = *src++ >> bits;
		c = pal[v & 0x0e];
		m = 0 - (v & 1);
		Grp_LineBufSP[i] = c & m;
		Grp_LineBufSP2[i] = c & ~m;
	}
}

// ---- translucent page (16 colours) ----

GRP_INLINE void Grp4_TR(const GWORD *src, DWORD i, DWORD end, const int sh, int opaq, int odd)
{
	const WORD *pal = GrphPal;
	DWORD v, v0, c;

	if (opaq) {
		for (; i < end; i++) {
			v = (*src++ >> sh) & 15;
			v0 = Grp_LineBufSP[i];
			c = pal[v];
			if (v0 != 0)
				c = (v != 0 && c != 0) ? Grp_Blend(v0, c) : 0;
			Grp_LineBuf[i] = (WORD)c;
		}
	} else {
		for (; i < end; i++) {
			v = (*src++ >> sh) & 15;
			v0 = Grp_LineBufSP[i];
			if (v0 == 0) {
				if (v != 0)
					Grp_LineBuf[i] = pal[v];
			} else if (v == 0) {
				Grp_LineBuf[i] = 0;
			} else {
				c = pal[v];
				if (c != 0) {
					c = Grp_Blend(v0, c);
					// NB: odd pages look the mixed colour up in
					// the palette again, as they always did
					Grp_LineBuf[i] = odd ? GrphPal[c] : (WORD)c;
				}
			}
		}
	}
}

static void Grp_DrawLine4TR_C(DWORD page, int opaq)
{
	const GWORD *line;
	DWORD x, i, end, n = TextDotX;

	page &= 3;
	line = (const GWORD *)(GVRAM + Grp_LineOfs(GrphScrollY[page]));
	x = GrphScrollX[page] & 0x1ff;

	for (i = 0; i < n; i = end, x = 0) {
		end = i + (512 - x);
		if (end > n)
			end = n;
		switch (page) {
		case 0: Grp4_TR(line + x, i, end, 0, opaq, 0); break;
		case 1: Grp4_TR(line + x, i, end, 4, opaq, 1); break;
		case 2: Grp4_TR(line + x, i, end, 8, opaq, 0); break;
		default: Grp4_TR(line + x, i, end, 12, opaq, 1); break;
		}
	}
}

// ---- 256 colours: low nibble from one plane, high nibble from another ----
//
// 'a' wraps back once, after 512-x dots; 'b' wraps at every line end.

GRP_INLINE void Grp8_Run(const BYTE *a, const BYTE *b, WORD *dst, DWORD n, int opaq)
{
	const WORD *pal = GrphPal;
	DWORD v;

	if (opaq) {
		for (; n; n--, a += 2, b += 2)
			*dst++ = pal[(*b & 0xf0) | (*a & 0x0f)];
	} else {
		for (; n; n--, a += 2, b += 2, dst++) {
			v = (*b & 0xf0) | (*a & 0x0f);
			if (v != 0)
				*dst = pal[v];
		}
	}
}

static void Grp_DrawLine8_C(int page, int opaq)
{
	const BYTE *a, *b;
	DWORD x, x0, i, end, awrap, bwrap, n = TextDotX;

	page &= 1;
	x = GrphScrollX[page * 2] & 0x1ff;
	x0 = GrphScrollX[page * 2 + 1] & 0x1ff;
	a = GVRAM + Grp_LineOfs(GrphScrollY[page * 2]) + page + x * 2;
	b = GVRAM + Grp_LineOfs(GrphScrollY[page * 2 + 1]) + page + x0 * 2;

	awrap = 512 - x;
	bwrap = 512 - x0;
	for (i = 0; i < n; i = end) {
		if (i == awrap)
			a -= 0x400;
		if (i == bwrap) {
			b -= 0x400;
			bwrap += 512;
		}
		end = n;
		if (awrap > i && awrap < end)
			end = awrap;
		if (bwrap < end)
			end = bwrap;
		Grp8_Run(a, b, Grp_LineBuf + i, end - i, opaq);
		a += (end - i) * 2;
		b += (end - i) * 2;
	}
}

GRP_INLINE void Grp8_SPRun(const BYTE *a, const BYTE *b, DWORD i, DWORD end)
{
	const WORD *pal = GrphPal;
	DWORD v, c;

	for (; i < end; i++, a += 2, b += 2) {
		v = (*a & 0x0f) | (*b & 0xf0);
		c = v & 0xfe;
		if (v & 1) {
			if (c != 0)
				c = pal[c] | Ibit;
			Grp_LineBufSP[i] = c;
			Grp_LineBufSP2[i] = 0;
		} else {
			if (c != 0)
				c = pal[c];
			Grp_LineBufSP[i] = 0;
			Grp_LineBufSP2[i] = c;
		}
	}
}

static void Grp_DrawLine8SP_C(int page)
{
	const BYTE *a, *b;
	DWORD x, x0, i, end, awrap, bwrap, n = TextDotX;

	page &= 1;
	x = GrphScrollX[page * 2] & 0x1ff;
	x0 = GrphScrollX[page * 2 + 1] & 0x1ff;
	a = GVRAM + Grp_LineOfs(GrphScrollY[page * 2]) + page + x * 2;
	b = GVRAM + Grp_LineOfs(GrphScrollY[page * 2 + 1]) + page + x0 * 2;

	awrap = 512 - x;
	bwrap = 512 - x0;
	for (i = 0; i < n; i = end) {
		if (i == awrap)
			a -= 0x400;
		if (i == bwrap) {
			b -= 0x400;
			bwrap += 512;
		}
		end = n;
		if (awrap > i && awrap < end)
			end = awrap;
		if (bwrap < end)
			end = bwrap;
		Grp8_SPRun(a, b, i, end);
		a += (end - i) * 2;
		b += (end - i) * 2;
	}
}

static void Grp_DrawLine8TR_C(int page, int opaq)
{
	const WORD *pal = GrphPal;
	const BYTE *line, *src;
	DWORD x, i, end, v, v0, c, n = TextDotX;

	if (!opaq)
		return;

	page &= 1;
	line = GVRAM + Grp_LineOfs(GrphScrollY[page * 2]) + page;
	x = GrphScrollX[page * 2] & 0x1ff;

	for (i = 0; i < n; x = 0) {
		end = i + (512 - x);
		if (end > n)
			end = n;
		for (src = line + x * 2; i < end; i++, src += 2) {
			v = *src;
			v0 = Grp_LineBufSP[i];
			c = pal[v];
			if (v0 != 0)
				c = (v != 0 && c != 0) ? Grp_Blend(v0, c) : 0;
			Grp_LineBuf[i] = (WORD)c;
		}
	}
}

// ---- 65536 colours ----

GRP_INLINE DWORD Grp16_Col(DWORD v)
{
	if (v != 0) {
		v = Pal_Regs[Pal16Adr[v & 0xff]]
		  | (Pal_Regs[Pal16Adr[v >> 8] + 2] << 8);
		v = Pal16[v];
	}
	return v;
}

static void Grp_DrawLine16_C(void)
{
	const GWORD *src;
	WORD *dst = Grp_LineBuf;
	DWORD x, n1, n = TextDotX;

	x = GrphScrollX[0] & 0x1ff;
	src = (const GWORD *)(GVRAM + Grp_LineOfs(GrphScrollY[0]) + x * 2);
	n1 = 512 - x;
	if (n1 >= n)
		n1 = n;
	n -= n1;

	for (; n1; n1--)
		*dst++ = Grp16_Col(*src++);
	src -= 0x200;
	for (; n; n--)
		*dst++ = Grp16_Col(*src++);
}

GRP_INLINE void Grp16_SP(DWORD w, DWORD i)
{
	DWORD lo = w & 0xff, hi = w >> 8, c;

	c = Pal16[((Pal_Regs[hi * 2] << 8) | Pal_Regs[lo * 2 + 1]) & 0xfffe];
	if (lo & 1) {
		Grp_LineBufSP[i] = c;
		Grp_LineBufSP2[i] = 0;
	} else {
		Grp_LineBufSP[i] = 0;
		Grp_LineBufSP2[i] = c;
	}
}

static void Grp_DrawLine16SP_C(void)
{
	const GWORD *src;
	DWORD x, i, n1, n = TextDotX;

	x = GrphScrollX[0] & 0x1ff;
	src = (const GWORD *)(GVRAM + Grp_LineOfs(GrphScrollY[0]) + x * 2);
	n1 = 512 - x;
	if (n1 >= n)
		n1 = n;

	for (i = 0; i < n1; i++)
		Grp16_SP(*src++, i);
	src -= 0x200;
	for (; i < n; i++)
		Grp16_SP(*src++, i);
}
#else

void FASTCALL Grp_DrawLine4Multi(DWORD pages, int n)
{
	int k;

	for (k = 0; k < n; k++)
		Grp_DrawLine4((pages >> (k * 2)) & 3, k == 0);
}
#endif /* !USE_ASM && !(USE_GAS && __i386__) */


// -----------------------------------------------------------------------
//   初期化〜
// -----------------------------------------------------------------------
void GVRAM_Init(void)
{
	int i;

	ZeroMemory(GVRAM, 0x80000);
	for (i=0; i<128; i++)			// 16bit color パレットアドレス計算用
	{
		Pal16Adr[i*2] = i*4;
		Pal16Adr[i*2+1] = i*4+1;
	}
}


// -----------------------------------------------------------------------------------
//  高速クリア用ルーチン
// -----------------------------------------------------------------------------------

void FASTCALL GVRAM_FastClear(void)
{
	DWORD v, h;
	v = ((CRTC_Regs[0x29]&4)?512:256);
	h = ((CRTC_Regs[0x29]&3)?512:256);
	// やっぱちゃんと範囲指定しないと変になるものもある（ダイナマイトデュークとか）
#ifdef USE_ASM
	_asm
	{
		push	ebx
		push	ecx
		push	edx
		push	esi
		mov	ax, CRTC_FastClrMask
		mov	ecx, v
		mov	esi, GrphScrollY[0]
		and	esi, 511
		shl	esi, 10
	fclp2:
		mov	edx, GrphScrollX[0]
		and	edx, 511
		mov	ebx, h
	fclp:
		and	word ptr GVRAM[esi+edx*2], ax
		inc	edx
		and	edx, 511
		dec	ebx
		jne	fclp
		add	esi, 1024
		and	esi, 07fc00h
		dec	ecx
		jne	fclp2
		pop	esi
		pop	edx
		pop	ecx
		pop	ebx
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushl	%%ebx;"
		"pushl	%%ecx;"
		"pushl	%%edx;"
		"pushl	%%esi;"
		"movw	%0, %%ax;"
		"movl	%3, %%ecx;"
		"movl	%1, %%esi;"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
	".fclp2:"
		"movl	%2, %%edx;"
		"andl	$511, %%edx;"
		"movl	%4, %%ebx;"
	".fclp:"
		"andw	%%ax, GVRAM(%%esi, %%edx, 2);"
		"incl	%%edx;"
		"andl	$511, %%edx;"
		"decl	%%ebx;"
		"jne	.fclp;"
		"addl	$1024, %%esi;"
		"andl	$0x7fc00, %%esi;"
		"decl	%%ecx;"
		"jne	.fclp2;"
		"popl	%%esi;"
		"popl	%%edx;"
		"popl	%%ecx;"
		"popl	%%ebx;"
	: /* output: nothing */
	: "m" (CRTC_FastClrMask), "m" (GrphScrollY[0]), "m" (GrphScrollX[0]),
	  "m" (v), "m" (h)
	: "eax");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
{
	WORD *p;
	DWORD x, y;
	DWORD offx, offy;

	offy = (GrphScrollY[0] & 0x1ff) << 10;
	for (y = 0; y < v; ++y) {
		offx = GrphScrollX[0] & 0x1ff;
		p = (WORD *)(GVRAM + offy + offx * 2);

		for (x = 0; x < h; ++x) {
			*p++ &= CRTC_FastClrMask;
			offx = (offx + 1) & 0x1ff;
		}

		offy = (offy + 0x400) & 0x7fc00;
	}
}
#endif /* USE_ASM */
}


// -----------------------------------------------------------------------
//   VRAM Read
// -----------------------------------------------------------------------
BYTE FASTCALL GVRAM_Read(DWORD adr)
{
	BYTE ret=0;
	BYTE page;
	WORD *ram = (WORD*)(&GVRAM[adr&0x7fffe]);
	adr ^= 1;
	adr -= 0xc00000;

	if (CRTC_Regs[0x28]&8) {			// 読み込み側も65536モードのVRAM配置（苦胃頭捕物帳）
		if (adr<0x80000) ret = GVRAM[adr];
	} else {
		switch(CRTC_Regs[0x28]&3)
		{
		case 0:					// 16 colors
			if (!(adr&1))
			{
				if (CRTC_Regs[0x28]&4)		// 1024dot
				{
					ram = (WORD*)(&GVRAM[((adr&0xff800)>>1)+(adr&0x3fe)]);
					page = (BYTE)((adr>>17)&0x08);
					page += (BYTE)((adr>>8)&4);
					ret = (((*ram)>>page)&15);
				}
				else
				{
					page = (BYTE)((adr>>17)&0x0c);
					ret = (((*ram)>>page)&15);
				}
			}
			break;
		case 1:					// 256
		case 2:					// Unknown
			if ( adr<0x100000 )
			{
				if (!(adr&1))
				{
					page = (BYTE)((adr>>16)&0x08);
					ret = (BYTE)((*ram)>>page);
				}
			}
//			else
//				BusErrFlag = 1;
			break;
		case 3:					// 65536
			if (adr<0x80000)
				ret = GVRAM[adr];
//			else
//				BusErrFlag = 1;
			break;
		}
	}
	return ret;
}


// -----------------------------------------------------------------------
//   VRAM Write
// -----------------------------------------------------------------------
void FASTCALL GVRAM_Write(DWORD adr, BYTE data)
{
	BYTE page;
	int line = 1023, scr = 0;
	WORD *ram = (WORD*)(&GVRAM[adr&0x7fffe]);
	WORD temp;

	adr ^= 1;
	adr -= 0xc00000;


	if (CRTC_Regs[0x28]&8)				// 65536モードのVRAM配置？（Nemesis）
	{
		if ( adr<0x80000 )
		{
			GVRAM[adr] = data;
			line = (((adr&0x7ffff)/1024)-GrphScrollY[0])&511;
		}
	}
	else
	{
		switch(CRTC_Regs[0x28]&3)
		{
		case 0:					// 16 colors
			if (adr&1) break;
			if (CRTC_Regs[0x28]&4)		// 1024dot
			{
				ram = (WORD*)(&GVRAM[((adr&0xff800)>>1)+(adr&0x3fe)]);
				page = (BYTE)((adr>>17)&0x08);
				page += (BYTE)((adr>>8)&4);
				temp = ((WORD)data&15)<<page;
				*ram = ((*ram)&(~(0xf<<page)))|temp;
				line = ((adr/2048)-GrphScrollY[0])&1023;
			}
			else
			{
				page = (BYTE)((adr>>17)&0x0c);
				temp = ((WORD)data&15)<<page;
				*ram = ((*ram)&(~(0xf<<page)))|temp;
				switch(adr/0x80000)
				{
					case 0:	scr = GrphScrollY[0]; break;
					case 1: scr = GrphScrollY[1]; break;
					case 2: scr = GrphScrollY[2]; break;
					case 3: scr = GrphScrollY[3]; break;
				}
				line = (((adr&0x7ffff)/1024)-scr)&511;
			}
			break;
		case 1:					// 256 colors
		case 2:					// Unknown
			if ( adr<0x100000 )
			{
				if ( !(adr&1) )
				{
					scr = GrphScrollY[(adr>>18)&2];
					line = (((adr&0x7ffff)>>10)-scr)&511;
					TextDirtyLine[line] = 1;			// 32色4面みたいな使用方法時
					scr = GrphScrollY[((adr>>18)&2)+1];		//
					line = (((adr&0x7ffff)>>10)-scr)&511;		//
					if (adr&0x80000) adr+=1;
					adr &= 0x7ffff;
					GVRAM[adr] = data;
				}
			}
//			else
//			{
//				BusErrFlag = 1;
//				return;
//			}
			break;
		case 3:					// 65536 colors
			if ( adr<0x80000 )
			{
				GVRAM[adr] = data;
				line = (((adr&0x7ffff)>>10)-GrphScrollY[0])&511;
			}
//			else
//			{
//				BusErrFlag = 1;
//				return;
//			}
			break;
		}
		TextDirtyLine[line] = 1;
	}
}

/*
 * A CPU word write (adr even, $c00000-$dfffff): the same as
 * GVRAM_Write(adr, data >> 8) then GVRAM_Write(adr + 1, data & 0xff), as
 * mem_wrap.c did, in one go (3D games fill GVRAM a word at a time).
 */
typedef WORD __attribute__((may_alias)) GVRAM_WORD;

void FASTCALL GVRAM_WriteWord(DWORD adr, WORD data)
{
	DWORD a = adr - 0xc00000;	/* the low byte's offset; the high byte is at a + 1 */
	const BYTE r28 = CRTC_Regs[0x28];

	if ( (r28&8) || (r28&3)==3 ) {		/* 65536 colours: both bytes stored */
		if ( a<0x80000 ) {
			*(GVRAM_WORD *)&GVRAM[a] = data;
			if ( !(r28&8) )
				TextDirtyLine[((a>>10)-GrphScrollY[0])&511] = 1;
		} else if ( !(r28&8) )
			TextDirtyLine[1023] = 1;
		return;
	}
	if ( !(r28&4) ) {
		/*
		 * 16 / 256 colours, 512 dots: the high byte (odd offset in
		 * GVRAM_Write) is not stored, only line 1023 is marked.
		 */
		TextDirtyLine[1023] = 1;
		GVRAM_Write(adr + 1, (BYTE)data);
		return;
	}
	GVRAM_Write(adr, (BYTE)(data >> 8));
	GVRAM_Write(adr + 1, (BYTE)data);
}


// -----------------------------------------------------------------------
//   こっから後はライン単位での画面展開部
// -----------------------------------------------------------------------
LABEL void Grp_DrawLine16(void)
{
#ifdef USE_ASM
	__asm {
			pushf
			cld
			push	es
			mov	ax, ds
			mov	es, ax
			push	ebx
			push	esi
			push	edi
			mov	esi, GrphScrollY[0]
			add	esi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp16linenotspecial
			add	esi, VLINE
		gp16linenotspecial:
			and	esi, 511
			shl	esi, 10
			mov	ebx, GrphScrollX[0]
			and	ebx, 511
			lea	esi, GVRAM[esi+ebx*2]
			xor	ebx, 511
			inc	bx
			mov	ecx, TextDotX
			mov	edi, offset Grp_LineBuf
			xor	eax, eax
			xor	edx, edx
			cmp	ecx, ebx
			jbe	gp16linelp_b
			sub	ecx, ebx
		gp16linelp_a:
			lodsw
			or	ax, ax
			je	gp16linelp_a_skip
			mov	dl, ah
			mov	ah, 0
			mov	dh, 0
			mov	ax, word ptr Pal16Adr[eax*2]
			mov	dx, word ptr Pal16Adr[edx*2]
			mov	al, byte ptr Pal_Regs[eax]
			mov	ah, byte ptr Pal_Regs[edx+2]
			mov	ax, word ptr Pal16[eax*2]
;or ax, Ibit		; 20010120
		gp16linelp_a_skip:
			stosw
			dec	bx
			jnz	gp16linelp_a
			sub	esi, 400h
		gp16linelp_b:
			lodsw
			or	ax, ax
			je	gp16linelp_b_skip
			mov	dl, ah
			mov	ah, 0
			mov	dh, 0
			mov	ax, word ptr Pal16Adr[eax*2]
			mov	dx, word ptr Pal16Adr[edx*2]
			mov	al, byte ptr Pal_Regs[eax]
			mov	ah, byte ptr Pal_Regs[edx+2]
			mov	ax, word ptr Pal16[eax*2]
;or ax, Ibit		; 20010120
		gp16linelp_b_skip:
			stosw
			loop	gp16linelp_b
			pop	edi
			pop	esi
			pop	ebx
			pop	es
			popf
			ret
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushf;"
		"cld;"

#if 0	/* まあ、同じセグメント差してる筈だし… */
		"pushl	%%es;"

		"movw	%%ds, %%ax;"
		"movw	%%ax, %%es;"
#endif
		"pushl	%%ebx;"
		"pushl	%%esi;"
		"pushl	%%edi;"
		"movl	%0, %%esi;"
		"addl	%1, %%esi;"
		"movb	%2, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp16linenotspecial;"
		"addl	%1, %%esi;"
	".gp16linenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	%3, %%ebx;"
		"andl	$511, %%ebx;"
		"leal	GVRAM(%%esi, %%ebx, 2), %%esi;"
		"xorl	$511, %%ebx;"
		"incw	%%bx;"
		"movl	%4, %%ecx;"
		"movl	%5, %%edi;"
		"xorl	%%eax, %%eax;"
		"xorl	%%edx, %%edx;"
		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp16linelp_b;"
		"subl	%%ebx, %%ecx;"
	".gp16linelp_a:"
		"lodsw;"
		"orw	%%ax, %%ax;"
		"je	.gp16linelp_a_skip;"
		"movb	%%ah, %%dl;"
		"movb	$0, %%ah;"
		"movb	$0, %%dh;"
		"movw	Pal16Adr(, %%eax, 2), %%ax;"
		"movw	Pal16Adr(, %%edx, 2), %%dx;"
		"movb	Pal_Regs(%%eax), %%al;"
		"movb	Pal_Regs + 2(%%edx), %%ah;"
		"movw	Pal16(, %%eax, 2), %%ax;"
	".gp16linelp_a_skip:"
		"stosw;"
		"decw	%%bx;"
		"jnz	.gp16linelp_a;"
		"subl	$0x400, %%esi;"
	".gp16linelp_b:"
		"lodsw;"
		"orw	%%ax, %%ax;"
		"je	.gp16linelp_b_skip;"
		"movb	%%ah, %%dl;"
		"movb	$0, %%ah;"
		"movb	$0, %%dh;"
		"movw	Pal16Adr(, %%eax, 2), %%ax;"
		"movw	Pal16Adr(, %%edx, 2), %%dx;"
		"movb	Pal_Regs(%%eax), %%al;"
		"movb	Pal_Regs + 2(%%edx), %%ah;"
		"movw	Pal16(, %%eax, 2), %%ax;"
	".gp16linelp_b_skip:"
		"stosw;"
		"loop	.gp16linelp_b;"

#if 0	/* まあ、同じセグメント差してる筈だし… */
		"popl	%%es;"
#endif
		"popl	%%edi;"
		"popl	%%esi;"
		"popl	%%ebx;"
		"popf;"
	: /* output: nothing */
	: "m" (GrphScrollY[0]), "m" (VLINE), "m" (CRTC_Regs[0x29]),
	  "m" (GrphScrollX[0]), "m" (TextDotX), "g" (Grp_LineBuf)
	: "ax", "cx", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine16_C();
#endif /* USE_ASM */
}


LABEL void FASTCALL Grp_DrawLine8(int page, int opaq)
{
#ifdef USE_ASM
	__asm {
			pushf
			cld
			push	ebx
			push	ecx
			push	esi
			push	edi

			and	ecx, 1		// ecx = page
			mov	esi, GrphScrollY[ecx*8]
			mov	edi, GrphScrollY[ecx*8+4]
			add	esi, VLINE
			add	edi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp8linenotspecial
			add	esi, VLINE
			add	edi, VLINE
		gp8linenotspecial:
			and	esi, 511
			shl	esi, 10
			add	esi, ecx
			and	edi, 511
			shl	edi, 10
			add	edi, ecx
			mov	ebx, GrphScrollX[ecx*8+4]
			and	ebx, 511
			add	edi, ebx
			add	edi, ebx
			mov	ebx, GrphScrollX[ecx*8]
			and	ebx, 511
			lea	esi, GVRAM[esi+ebx*2]
			xor	ebx, 511
			inc	bx

			xor	eax, eax
			mov	ecx, TextDotX

			or	edx, edx	// edx = opaq
			mov	edx, offset Grp_LineBuf
			je	gp8linelp

			cmp	ecx, ebx
			jbe	gp8olinelp_b
			sub	ecx, ebx

		gp8olinelp_a:
			lodsw
			mov ah, byte ptr GVRAM[edi]
			and ah, 0f0h
			and al, 0fh
			or al, ah
			mov	ah, 0
;or al,al
			mov	ax, word ptr GrphPal[eax*2]
;jz gp8noti_a
;or ax, Ibit		; 20010120
;gp8noti_a:
			mov	[edx], ax
			add	edx, 2
			add	edi, 2
			test	di, 03feh
			jnz	gp8onotxend1
			sub	edi, 0400h
		gp8onotxend1:
			dec	bx
			jnz	gp8olinelp_a
			sub	esi, 400h
		gp8olinelp_b:
			lodsw
			mov ah, byte ptr GVRAM[edi]
			and ah, 0f0h
			and al, 0fh
			or al, ah
			mov	ah, 0
;or al,al
			mov	ax, word ptr GrphPal[eax*2]
;jz gp8noti_b
;or ax, Ibit		; 20010120
;gp8noti_b:
			mov	[edx], ax
			add	edx, 2
			add	edi, 2
			test	di, 03feh
			jnz	gp8onotxend2
			sub	edi, 0400h
		gp8onotxend2:
			loop	gp8olinelp_b

			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
			popf
			ret

		gp8linelp:
			cmp	ecx, ebx
			jbe	gp8linelp_b
			sub	ecx, ebx
		gp8linelp_a:
			lodsw
			mov ah, byte ptr GVRAM[edi]
			and ah, 0f0h
			and al, 0fh
			or al, ah
			and	ax, 00ffh
			jz	gp8lineskip_a
			mov	ax, word ptr GrphPal[eax*2]
;or ax, Ibit		; 20010120
			mov	[edx], ax
		gp8lineskip_a:
			add	edx, 2
			add	edi, 2
			test	di, 03feh
			jnz	gp8notxend1
			sub	edi, 0400h
		gp8notxend1:
			dec	bx
			jnz	gp8linelp_a
			sub	esi, 400h
		gp8linelp_b:
			lodsw
			mov ah, byte ptr GVRAM[edi]
			and ah, 0f0h
			and al, 0fh
			or al, ah
			and	ax, 00ffh
			jz	gp8lineskip_b
			mov	ax, word ptr GrphPal[eax*2]
;or ax, Ibit		; 20010120
			mov	[edx], ax
		gp8lineskip_b:
			add	edx, 2
			add	edi, 2
			test	di, 03feh
			jnz	gp8notxend2
			sub	edi, 0400h
		gp8notxend2:
			loop	gp8linelp_b

			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
			popf
			ret
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushf;"
		"cld;"
		"pushl	%%ebx;"
		"pushl	%%ecx;"
		"pushl	%%esi;"
		"pushl	%%edi;"

		"movl	%0, %%ecx;"	// page
		"movl	%1, %%edx;"	// opaq

		"andl	$1, %%ecx;"	// ecx = page
		"movl	GrphScrollY(, %%ecx, 8), %%esi;"
		"movl	GrphScrollY + 4(, %%ecx, 8), %%edi;"
		"addl	%2, %%esi;"
		"addl	%2, %%edi;"
		"movb	%3, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp8linenotspecial;"
		"addl	%2, %%esi;"
		"addl	%2, %%edi;"
	".gp8linenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"addl	%%ecx, %%esi;"
		"andl	$511, %%edi;"
		"shll	$10, %%edi;"
		"addl	%%ecx, %%edi;"
		"movl	GrphScrollX + 4(, %%ecx, 8), %%ebx;"
		"andl	$511, %%ebx;"
		"addl	%%ebx, %%edi;"
		"addl	%%ebx, %%edi;"
		"movl	GrphScrollX(, %%ecx, 8), %%ebx;"
		"andl	$511, %%ebx;"
		"leal	GVRAM(%%esi, %%ebx, 2), %%esi;"
		"xorl	$511, %%ebx;"
		"incw	%%bx;"

		"xorl	%%eax, %%eax;"
		"movl	%4, %%ecx;"

		"orl	%%edx, %%edx;"	// edx = opaq
		"movl	%5, %%edx;"
		"je	.gp8linelp;"

		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp8olinelp_b;"
		"subl	%%ebx, %%ecx;"

	".gp8olinelp_a:"
		"lodsw;"
		"movb	GVRAM(%%edi), %%ah;"
		"andb	$0xf0, %%ah;"
		"andb	$0x0f, %%al;"
		"orb	%%ah, %%al;"
		"movb	$0, %%ah;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
		"addl	$2, %%edx;"
		"addl	$2, %%edi;"
		"testw	$0x3fe, %%di;"
		"jnz	.gp8onotxend1;"
		"subl	$0x400, %%edi;"
	".gp8onotxend1:"
		"decw	%%bx;"
		"jnz	.gp8olinelp_a;"
		"subl	0x400, %%esi;"
	".gp8olinelp_b:"
		"lodsw;"
		"movb	GVRAM(%%edi), %%ah;"
		"andb	$0xf0, %%ah;"
		"andb	$0x0f, %%al;"
		"orb	%%ah, %%al;"
		"movb	$0, %%ah;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
		"addl	$2, %%edx;"
		"addl	$2, %%edi;"
		"testw	$0x3fe, %%di;"
		"jnz	.gp8onotxend2;"
		"subl	$0x400, %%edi;"
	".gp8onotxend2:"
		"loop	.gp8olinelp_b;"
		"jmp	.gp8end;"

	".gp8linelp:"
		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp8linelp_b;"
		"subl	%%ebx, %%ecx;"
	".gp8linelp_a:"
		"lodsw;"
		"movb	GVRAM(%%edi), %%ah;"
		"andb	$0xf0, %%ah;"
		"andb	$0x0f, %%al;"
		"orb	%%ah, %%al;"
		"andw	$0xff, %%ax;"
		"jz	.gp8lineskip_a;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
	".gp8lineskip_a:"
		"addl	$2, %%edx;"
		"addl	$2, %%edi;"
		"testw	$0x3fe, %%di;"
		"jnz	.gp8notxend1;"
		"subl	$0x400, %%edi;"
	".gp8notxend1:"
		"decw	%%bx;"
		"jnz	.gp8linelp_a;"
		"subl	$0x400, %%esi;"
	".gp8linelp_b:"
		"lodsw;"
		"movb	GVRAM(%%edi), %%ah;"
		"andb	$0xf0, %%ah;"
		"andb	$0x0f, %%al;"
		"orb	%%ah, %%al;"
		"andw	$0xff, %%ax;"
		"jz	.gp8lineskip_b;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
	".gp8lineskip_b:"
		"addl	$2, %%edx;"
		"addl	$2, %%edi;"
		"testw	$0x3fe, %%di;"
		"jnz	.gp8notxend2;"
		"subl	$0x400, %%edi;"
	".gp8notxend2:"
		"loop	.gp8linelp_b;"

	".gp8end:"
		"popl	%%edi;"
		"popl	%%esi;"
		"popl	%%ecx;"
		"popl	%%ebx;"
		"popf;"
	: /* output: nothing */
	: "m" (page), "m" (opaq), "m" (VLINE), "m" (CRTC_Regs[0x29]), 
	  "m" (TextDotX), "g" (Grp_LineBuf)
	: "ax", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine8_C(page, opaq);
#endif /* USE_ASM */
}

				// Manhattan Requiem Opening 7.0→7.5MHz
LABEL void FASTCALL Grp_DrawLine4(DWORD page, int opaq)
{
#ifdef USE_ASM
	__asm {
			pushf
			cld
			push	ebx
			push	ecx
			push	esi
			and	ecx, 3
			mov	esi, GrphScrollY[ecx*4]
			add	esi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp4linenotspecial
			add	esi, VLINE
		gp4linenotspecial:
			and	esi, 511
			shl	esi, 10
			mov	ebx, GrphScrollX[ecx*4]
			and	ebx, 511
			lea	esi, [esi+ebx*2]
			xor	ebx, 511
			inc	bx
			xor	eax, eax

			shr	cl, 1
			lea	esi, GVRAM[esi+ecx]	// ecx = page/2
			jnc	gp4olinepage0		// (shr cl,1) page0or2

			mov	ecx, TextDotX
			or	edx, edx
			mov	edx, offset Grp_LineBuf
			jz	gp4linelp2		// opaq == 0

			cmp	ecx, ebx
			jbe	gp4olinelp2_b
			sub	ecx, ebx
		gp4olinelp2_a:
			lodsw
			mov	ah, 0
			shr	al, 4
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
			add	edx, 2
			dec	bx
			jnz	gp4olinelp2_a
			sub	esi, 400h
		gp4olinelp2_b:
			lodsw
			mov	ah, 0
			shr	al, 4
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
			add	edx, 2
			loop	gp4olinelp2_b
			pop	esi
			pop	ecx
			pop	ebx
			popf
			ret

		gp4linelp2:
			cmp	ecx, ebx
			jbe	gp4linelp2_b
			sub	ecx, ebx
		gp4linelp2_a:
			lodsw
			mov	ah, 0
			shr	al, 4			// shrのZFは確認済
			jz	gp4lineskip2_a
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
		gp4lineskip2_a:
			add	edx, 2
			dec	bx
			jnz	gp4linelp2_a
			sub	esi, 400h
		gp4linelp2_b:
			lodsw
			mov	ah, 0
			shr	al, 4			// shrのZFは確認済
			jz	gp4lineskip2_b
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
		gp4lineskip2_b:
			add	edx, 2
			loop	gp4linelp2_b
			pop	esi
			pop	ecx
			pop	ebx
			popf
			ret

		gp4olinepage0:
			mov	ecx, TextDotX
			or	edx, edx
			mov	edx, offset Grp_LineBuf
			jz	gp4linelp		// opaq == 0

			cmp	ecx, ebx
			jbe	gp4olinelp_b
			sub	ecx, ebx
		gp4olinelp_a:
			lodsw
			and	ax, 15
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
			add	edx, 2
			dec	bx
			jnz	gp4olinelp_a
			sub	esi, 400h
		gp4olinelp_b:
			lodsw
			and	ax, 15
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
			add	edx, 2
			loop	gp4olinelp_b
			pop	esi
			pop	ecx
			pop	ebx
			popf
			ret

		gp4linelp:
			cmp	ecx, ebx
			jbe	gp4linelp_b
			sub	ecx, ebx
		gp4linelp_a:
			lodsw
			and	ax, 15
			jz	gp4lineskip_a
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
		gp4lineskip_a:
			add	edx, 2
			dec	bx
			jnz	gp4linelp_a
			sub	esi, 400h
		gp4linelp_b:
			lodsw
			and	ax, 15
			jz	gp4lineskip_b
			mov	ax, word ptr GrphPal[eax*2]
			mov	[edx], ax
		gp4lineskip_b:
			add	edx, 2
			loop	gp4linelp_b
			pop	esi
			pop	ecx
			pop	ebx
			popf
			ret
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushf;"
		"cld;"
		"pushl	%%ebx;"
		"pushl	%%ecx;"
		"pushl	%%esi;"
		"pushl	%%edi;"
	
		"movl	%0, %%ecx;"	// page
		"movl	%1, %%edx;"	// opaq

		"andl	$3, %%ecx;"	// ecx = page
		"movl	GrphScrollY(, %%ecx, 4), %%esi;"
		"addl	%2, %%esi;"
		"movb	%3, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp4linenotspecial;"
		"addl	%2, %%esi;"
	".gp4linenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	GrphScrollX(, %%ecx, 4), %%ebx;"
		"andl	$511, %%ebx;"
		"leal	(%%esi, %%ebx, 2), %%esi;"
		"xorl	$511, %%ebx;"
		"incw	%%bx;"
		"xorl	%%eax, %%eax;"

		"shrb	$1, %%cl;"
		"leal	GVRAM(%%esi, %%ecx), %%esi;"	// ecx = page/2
		"jnc	.gp4olinepage0;"		// (shr cl,1) page0or2

		"movl	%4, %%ecx;"
		"orl	%%edx, %%edx;"
		"movl	%5, %%edx;"
		"jz	.gp4linelp2;"			// opaq == 0

		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp4olinelp2_b;"
		"subl	%%ebx, %%ecx;"
	".gp4olinelp2_a:"
		"lodsw;"
		"movb	$0, %%ah;"
		"shrb	$4, %%al;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
		"addl	$2, %%edx;"
		"decw	%%bx;"
		"jnz	.gp4olinelp2_a;"
		"subl	$0x400, %%esi;"
	".gp4olinelp2_b:"
		"lodsw;"
		"movb	$0, %%ah;"
		"shrb	$4, %%al;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
		"addl	$2, %%edx;"
		"loop	.gp4olinelp2_b;"
		"jmp	.gp4end;"

	".gp4linelp2:"
		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp4linelp2_b;"
		"subl	%%ebx, %%ecx;"
	".gp4linelp2_a:"
		"lodsw;"
		"movb	$0, %%ah;"
		"shrb	$4, %%al;"			// shrのZFは確認済
		"jz	.gp4lineskip2_a;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
	".gp4lineskip2_a:"
		"addl	$2, %%edx;"
		"decw	%%bx;"
		"jnz	.gp4linelp2_a;"
		"subl	$0x400, %%esi;"
	".gp4linelp2_b:"
		"lodsw;"
		"movb	$0,%%ah;"
		"shrb	$4, %%al;"			// shrのZFは確認済
		"jz	.gp4lineskip2_b;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
	".gp4lineskip2_b:"
		"addl	$2, %%edx;"
		"loop	.gp4linelp2_b;"
		"jmp	.gp4end;"

	".gp4olinepage0:"
		"movl	(%4), %%ecx;"
		"orl	%%edx, %%edx;"
		"movl	%5, %%edx;"
		"jz	.gp4linelp;"		// opaq == 0

		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp4olinelp_b;"
		"subl	%%ebx, %%ecx;"
	".gp4olinelp_a:"
		"lodsw;"
		"andw	$15, %%ax;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
		"addl	$2, %%edx;"
		"decw	%%bx;"
		"jnz	.gp4olinelp_a;"
		"subl	$0x400, %%esi;"
	".gp4olinelp_b:"
		"lodsw;"
		"andw	$15, %%ax;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
		"addl	$2, %%edx;"
		"loop	.gp4olinelp_b;"
		"jmp	.gp4end;"

	".gp4linelp:"
		"cmpl	%%ebx, %%ecx;"
		"jbe	.gp4linelp_b;"
		"subl	%%ebx, %%ecx;"
	".gp4linelp_a:"
		"lodsw;"
		"andw	$15, %%ax;"
		"jz	.gp4lineskip_a;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
	".gp4lineskip_a:"
		"addl	$2, %%edx;"
		"decw	%%bx;"
		"jnz	.gp4linelp_a;"
		"subl	$0x400, %%esi;"
	".gp4linelp_b:"
		"lodsw;"
		"andw	$15, %%ax;"
		"jz	.gp4lineskip_b;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, (%%edx);"
	".gp4lineskip_b:"
		"addl	$2, %%edx;"
		"loop	.gp4linelp_b;"

	".gp4end:"
		"popl	%%edi;"
		"popl	%%esi;"
		"popl	%%ecx;"
		"popl	%%ebx;"
		"popf;"
	: /* output: nothing */
	: "m" (page), "m" (opaq), "m" (VLINE), "m" (CRTC_Regs[0x29]), 
	  "m" (TextDotX), "g" (Grp_LineBuf)
	: "ax", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine4_C(page, opaq);
#endif /* USE_ASM */
}

					// この画面モードは勘弁して下さい…
void FASTCALL Grp_DrawLine4h(void)
{
#ifdef USE_ASM
	__asm
	{
		push	esi
		push	edi
		mov	esi, GrphScrollY[0]
		add	esi, VLINE
		mov	al, CRTC_Regs[0x29]
		and	al, 1ch
		cmp	al, 1ch
		jne	gp4hlinenotspecial
		add	esi, VLINE
	gp4hlinenotspecial:
		and	esi, 1023
		test	esi, 512
		jnz	gp4h_plane23
		shl	esi, 10
		mov	edx, GrphScrollX[0]
		mov	edi, edx
		and	edx, 511
		add	esi, edx
		add	esi, edx
		mov	cl, 00h
		test	edi, 512
		jz	gp4h_main
		add	cl, 4
		jmp	gp4h_main
	gp4h_plane23:
		and	esi, 511
		shl	esi, 10
		mov	edx, GrphScrollX[0]
		mov	edi, edx
		and	edx, 511
		add	esi, edx
		add	esi, edx
		mov	cl, 08h
		test	edi, 512
		jz	gp4h_main
		add	cl, 4
	gp4h_main:
		and	edi, 511
		xor	di, 511
		inc	di
		//and	di, 512
		mov	ebx, TextDotX
		xor	edx, edx
	gp4hlinelp:
		mov	ax, word ptr GVRAM[esi]
		shr	ax, cl
		and	eax, 15
		mov	ax, word ptr GrphPal[eax*2]
		mov	Grp_LineBuf[edx], ax
		add	esi, 2
		add	edx, 2
		dec	di
		jnz	gp4hline_cnt
		sub	esi, 0400h
		xor	cl, 4
		mov	di, 512
	gp4hline_cnt:
		dec	bx
		jnz	gp4hlinelp
//		loop	gp4hlinelp
		pop	edi
		pop	esi
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushl	%%esi;"
		"pushl	%%edi;"
		"movl	%0, %%esi;"
		"addl	%1, %%esi;"
		"movb	%2, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp4hlinenotspecial;"
		"addl	%1, %%esi;"
	".gp4hlinenotspecial:"
		"andl	$1023, %%esi;"
		"testl	$512, %%esi;"
		"jnz	.gp4h_plane23;"
		"shll	$10, %%esi;"
		"movl	%3, %%edx;"
		"movl	%%edx, %%edi;"
		"andl	$511, %%edx;"
		"addl	%%edx, %%esi;"
		"addl	%%edx, %%esi;"
		"movb	$0, %%cl;"
		"testl	$512, %%edi;"
		"jz	.gp4h_main;"
		"addb	$4, %%cl;"
		"jmp	.gp4h_main;"
	".gp4h_plane23:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	%3, %%edx;"
		"movl	%%edx, %%edi;"
		"andl	$511, %%edx;"
		"addl	%%edx, %%esi;"
		"addl	%%edx, %%esi;"
		"movb	$0x08, %%cl;"
		"testl	$512, %%edi;"
		"jz	.gp4h_main;"
		"addb	$4, %%cl;"
	".gp4h_main:"
		"andl	$511, %%edi;"
		"xorw	$511, %%di;"
		"incw	%%di;"
		"movl	%4, %%ebx;"
		"xorl	%%edx, %%edx;"
	".gp4hlinelp:"
		"movw	GVRAM(%%esi), %%ax;"
		"shrw	%%cl, %%ax;"
		"andl	$15, %%eax;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, Grp_LineBuf(%%edx);"
		"addl	$2, %%esi;"
		"addl	$2, %%edx;"
		"decw	%%di;"
		"jnz	.gp4hline_cnt;"
		"subl	$0x400, %%esi;"
		"xorb	$4, %%cl;"
		"movw	$512, %%di;"
	".gp4hline_cnt:"
		"decw	%%bx;"
		"jnz	.gp4hlinelp;"
		"popl	%%edi;"
		"popl	%%esi;"
	: /* output: nothing */
	: "m" (GrphScrollY[0]), "m" (VLINE), "m" (CRTC_Regs[0x29]), 
	  "m" (GrphScrollX[0]), "m" (TextDotX)
	: "ax", "bx", "cx", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine4h_C();
#endif /* !USE_ASM */
}


// -------------------------------------------------
// --- 半透明／特殊Priのベースとなるページの描画 ---
// -------------------------------------------------
void FASTCALL Grp_DrawLine16SP(void)
{
#ifdef USE_ASM
	__asm
	{
		push	edi
		push	esi
		mov	edi, 0
		mov	esi, GrphScrollY[0]
		add	esi, VLINE
		mov	al, CRTC_Regs[0x29]
		and	al, 1ch
		cmp	al, 1ch
		jne	gp16splinenotspecial
		add	esi, VLINE
	gp16splinenotspecial:
		and	esi, 511
		shl	esi, 10
		mov	ebx, GrphScrollX[0]
		and	ebx, 511
		add	esi, ebx
		add	esi, ebx
		xor	ebx, 511
		inc	ebx
		mov	ecx, TextDotX
		xor	edx, edx
	gp16splinelp:
		movzx	eax, byte ptr GVRAM[esi+1]
		mov	dh, byte ptr Pal_Regs[eax*2]
		movzx	eax, byte ptr GVRAM[esi]
		mov	dl, byte ptr Pal_Regs[eax*2+1]
		test	al, 1
		jnz	gp16splinesp
		and	dx, 0fffeh
		mov	ax, word ptr Pal16[edx*2]
		mov	word ptr Grp_LineBufSP[edi], 0
		mov	Grp_LineBufSP2[edi], ax
		jmp	gp16splineskip
	gp16splinesp:
		and	dx, 0fffeh
		mov	ax, word ptr Pal16[edx*2]
		mov	word ptr Grp_LineBufSP2[edi], 0
		mov	word ptr Grp_LineBufSP[edi], ax
	gp16splineskip:
		add	esi, 2
		add	edi, 2
		dec	ebx
		jnz	gp16spline_cnt
		sub	esi, 0400h
	gp16spline_cnt:
//		dec	cx
//		jnz	gp16splinelp
		loop	gp16splinelp
		pop	esi
		pop	edi
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushl	%%edi;"
		"pushl	%%esi;"
		"movl	$0, %%edi;"
		"movl	%0, %%esi;"
		"addl	%1, %%esi;"
		"movb	%2, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp16splinenotspecial;"
		"addl	%1, %%esi;"
	".gp16splinenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	%3, %%ebx;"
		"andl	$511, %%ebx;"
		"addl	%%ebx, %%esi;"
		"addl	%%ebx, %%esi;"
		"xorl	$511, %%ebx;"
		"incl	%%ebx;"
		"movl	%4, %%ecx;"
		"xorl	%%edx, %%edx;"
	".gp16splinelp:"
		"movzbl	GVRAM + 1(%%esi), %%eax;"
		"movb	Pal_Regs(, %%eax, 2), %%dh;"
		"movzbl	GVRAM(%%esi), %%eax;"
		"movb	Pal_Regs + 1(, %%eax, 2), %%dl;"
		"testb	$1, %%al;"
		"jnz	.gp16splinesp;"
		"andw	$0xfffe, %%dx;"
		"movw	Pal16(, %%edx, 2), %%ax;"
		"movw	$0, Grp_LineBufSP(%%edi);"
		"movw	%%ax, Grp_LineBufSP2(%%edi);"
		"jmp	.gp16splineskip;"
	".gp16splinesp:"
		"andw	$0xfffe, %%dx;"
		"movw	Pal16(, %%edx, 2), %%ax;"
		"movw	$0, Grp_LineBufSP2(%%edi);"
		"movw	%%ax, Grp_LineBufSP(%%edi);"
	".gp16splineskip:"
		"addl	$2, %%esi;"
		"addl	$2, %%edi;"
		"decl	%%ebx;"
		"jnz	.gp16spline_cnt;"
		"subl	$0x0400, %%esi;"
	".gp16spline_cnt:"
		"loop	.gp16splinelp;"
		"popl	%%esi;"
		"popl	%%edi;"
	: /* output: nothing */
	: "m" (GrphScrollY[0]), "m" (VLINE), "m" (CRTC_Regs[0x29]), 
	  "m" (GrphScrollX[0]), "m" (TextDotX)
	: "ax", "bx", "cx", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine16SP_C();
#endif /* USE_ASM */
}


void FASTCALL Grp_DrawLine8SP(int page)
{
#ifdef USE_ASM
		__asm
		{
			push	esi
push	edi
and	ecx, 1
mov	esi, GrphScrollY[ecx*8]
mov	edi, GrphScrollY[ecx*8+4]
			add	esi, VLINE
add	edi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp8splinenotspecial
			add	esi, VLINE
add	edi, VLINE
		gp8splinenotspecial:
			and	esi, 511
			shl	esi, 10
and	edi, 511
shl	edi, 10
mov	eax, GrphScrollX[ecx*8+4]
and	eax, 511
add	edi, eax
add	edi, eax
			mov	ebx, GrphScrollX[ecx*8]
			and	ebx, 511
			add	esi, ebx
			add	esi, ebx
add	esi, ecx
add	edi, ecx

			xor	bx, 511
			inc	bx
			mov	ecx, TextDotX
			xor	edx, edx
			xor	eax, eax
		gp8osplinelp:
			mov	al, byte ptr GVRAM[esi]
mov ah, byte ptr GVRAM[edi]
and ah, 0f0h
and al, 0fh
or al, ah
			//xor	ah, ah
			test	al, 1
			jnz	gp8osplinesp
			and	eax, 0feh
			jz	gp8onotzero2
			mov	ax, word ptr GrphPal[eax*2]
		gp8onotzero2:
			mov	word ptr Grp_LineBufSP[edx], 0
			mov	word ptr Grp_LineBufSP2[edx], ax
			jmp	gp8osplineskip
		gp8osplinesp:
			and	eax, 0feh
			jz	gp8onotzero			// ついんびー。Palette0以外の$0000は特殊Priでは透明じゃなく黒扱いらしい
			mov	ax, word ptr GrphPal[eax*2]
			or	ax, Ibit			// Palette0以外はIbit立ててごまかそー
		gp8onotzero:					// 半透明が変になる時は、半透明と特殊Priを別ルーチンにしなきゃ…
			mov	word ptr Grp_LineBufSP[edx], ax
			mov	word ptr Grp_LineBufSP2[edx], 0
		gp8osplineskip:
			add	esi, 2
add	edi, 2
test	di, 03feh
jnz	gp8spnotxend
sub	edi, 0400h
gp8spnotxend:
			add	edx, 2
			dec	bx
			jnz	gp8ospline_cnt
			sub	esi, 0400h
		gp8ospline_cnt:
			loop	gp8osplinelp
pop	edi
			pop	esi
		}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushl	%%esi;"
		"pushl	%%edi;"

		"movl	%0, %%ecx;"	// page

		"andl	$1, %%ecx;"
		"movl	GrphScrollY(, %%ecx, 8), %%esi;"
		"movl	GrphScrollY + 4(, %%ecx, 8), %%edi;"
		"addl	%1, %%esi;"
		"addl	%1, %%edi;"
		"movb	%2, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp8splinenotspecial;"
		"addl	%1, %%esi;"
		"addl	%1, %%edi;"
	".gp8splinenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"andl	$511, %%edi;"
		"shll	$10, %%edi;"
		"movl	GrphScrollX + 4(, %%ecx, 8), %%eax;"
		"andl	$511, %%eax;"
		"addl	%%eax, %%edi;"
		"addl	%%eax, %%edi;"
		"movl	GrphScrollX(, %%ecx, 8), %%ebx;"
		"andl	$511, %%ebx;"
		"addl	%%ebx, %%esi;"
		"addl	%%ebx, %%esi;"
		"addl	%%ecx, %%esi;"
		"addl	%%ecx, %%edi;"

		"xorw	$511, %%bx;"
		"incw	%%bx;"
		"movl	%2, %%ecx;"
		"xorl	%%edx, %%edx;"
		"xorl	%%eax, %%eax;"
	".gp8osplinelp:"
		"movb	GVRAM(%%esi), %%al;"
		"movb	GVRAM(%%edi), %%ah;"
		"andb	$0x0f0, %%ah;"
		"andb	$0x0f, %%al;"
		"orb	%%ah, %%al;"
		"testb	$1, %%al;"
		"jnz	.gp8osplinesp;"
		"andl	$0x0fe, %%eax;"
		"jz	.gp8onotzero2;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
	".gp8onotzero2:"
		"movw	$0, Grp_LineBufSP(%%edx);"
		"movw	%%ax, Grp_LineBufSP2(%%edx);"
		"jmp	.gp8osplineskip;"
	".gp8osplinesp:"
		"andl	$0x0fe, %%eax;"
		"jz	.gp8onotzero;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"or	%4, %%ax;"
	".gp8onotzero:"
		"movw	%%ax, Grp_LineBufSP(%%edx);"
		"movw	$0, Grp_LineBufSP2(%%edx);"
	".gp8osplineskip:"
		"addl	$2, %%esi;"
		"addl	$2, %%edi;"
		"testw	$0x03fe, %%di;"
		"jnz	.gp8spnotxend;"
		"subl	$0x0400, %%edi;"
	".gp8spnotxend:"
		"addl	$2, %%edx;"
		"decw	%%bx;"
		"jnz	.gp8ospline_cnt;"
		"subl	$0x0400, %%esi;"
	".gp8ospline_cnt:"
		"loop	.gp8osplinelp;"

		"popl	%%edi;"
		"popl	%%esi;"
	: /* output: nothing */
	: "m" (page), "m" (VLINE), "m" (CRTC_Regs[0x29]), "m" (TextDotX),
	  "m" (Ibit)
	: "ax", "bx", "cx", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine8SP_C(page);
#endif /* USE_ASM */
}


void FASTCALL Grp_DrawLine4SP(DWORD page/*, int opaq*/)
{
	DWORD scrx, scry;
	page &= 3;
	switch(page)		// 美しくなさすぎる（笑）
	{
	case 0:	scrx = GrphScrollX[0]; scry = GrphScrollY[0]; break;
	case 1: scrx = GrphScrollX[1]; scry = GrphScrollY[1]; break;
	case 2: scrx = GrphScrollX[2]; scry = GrphScrollY[2]; break;
	case 3: scrx = GrphScrollX[3]; scry = GrphScrollY[3]; break;
	}

#ifdef USE_ASM
	if (page&1)
	{
//	if (opaq)
//	{
		__asm
		{
			push	esi
			mov	esi, scry
			add	esi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp4splinenotspecial
			add	esi, VLINE
		gp4splinenotspecial:
			and	esi, 511
			shl	esi, 10
			mov	ebx, scrx
			and	ebx, 511
			add	esi, ebx
			add	esi, ebx
//			add	esi, vram
			test	page, 2
			jz	gp4ospline2page0
			inc	esi
		gp4ospline2page0:
			xor	bx, 511
			inc	bx
			mov	ecx, TextDotX
			xor	edx, edx
		gp4osplinelp2:
			mov	al, byte ptr GVRAM[esi]
			shr	al, 4
			test	al, 1
			jnz	gp4o2splinesp
			and	eax, 14
			mov	ax, word ptr GrphPal[eax*2]
			mov	word ptr Grp_LineBufSP[edx], 0
			mov	Grp_LineBufSP2[edx], ax
			jmp	gp4o2splineskip
		gp4o2splinesp:
			and	eax, 14
			mov	ax, word ptr GrphPal[eax*2]
			mov	word ptr Grp_LineBufSP[edx], ax
			mov	word ptr Grp_LineBufSP2[edx], 0
		gp4o2splineskip:
			add	esi, 2
			add	edx, 2
			dec	bx
			jnz	gp4ospline_cnt2
			sub	esi, 0400h
		gp4ospline_cnt2:
//			dec	cx
//			jnz	gp4osplinelp2
			loop	gp4osplinelp2
			pop	esi
		}
/*	}
	else
	{
		__asm
		{
			push	esi
			mov	esi, scry
			add	esi, VLINE
			and	esi, 511
			shl	esi, 10
			mov	ebx, scrx
			and	ebx, 511
			add	esi, ebx
			add	esi, ebx
//			add	esi, vram
			test	page, 2
			jz	gp4spline2page0
			inc	esi
		gp4spline2page0:
			xor	bx, 511
			inc	bx
			mov	ecx, TextDotX
			xor	edx, edx
		gp4splinelp2:
			mov	al, byte ptr GVRAM[esi]
			shr	al, 4
			test	al, 1
			jnz	gp4splinesp2
			and	eax, 14
			jz	gp4splineskip2_2
			mov	ax, word ptr GrphPal[eax*2]
			mov	Grp_LineBuf[edx], ax
		gp4splineskip2_2:
			mov	word ptr Grp_LineBufSP[edx], 0
			jmp	gp4splineskip2
		gp4splinesp2:
			and	eax, 14
			jz	gp4splineskip2_2
			mov	ax, word ptr GrphPal[eax*2]
//			mov	Grp_LineBuf[edx], ax
			mov	word ptr Grp_LineBufSP[edx], ax
		gp4splineskip2:
			add	esi, 2
			add	edx, 2
			dec	bx
			jnz	gp4spline_cnt2
			sub	esi, 0400h
		gp4spline_cnt2:
//			dec	cx
//			jnz	gp4splinelp2
			loop	gp4splinelp2
			pop	esi
		}
	}
*/
	}
	else
	{
//	if (opaq)
//	{
		__asm
		{
			push	esi
			mov	esi, scry
			add	esi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp4osplinenotspecial
			add	esi, VLINE
		gp4osplinenotspecial:
			and	esi, 511
			shl	esi, 10
			mov	ebx, scrx
			and	ebx, 511
			add	esi, ebx
			add	esi, ebx
//			add	esi, vram
			test	page, 2
			jz	gp4osplinepage0
			inc	esi
		gp4osplinepage0:
			xor	bx, 511
			inc	bx
			mov	ecx, TextDotX
			xor	edx, edx
		gp4osplinelp:
			mov	al, byte ptr GVRAM[esi]
			test	al, 1
			jnz	gp4osplinesp
			and	eax, 14
			mov	ax, word ptr GrphPal[eax*2]
			mov	word ptr Grp_LineBufSP[edx], 0
			mov	Grp_LineBufSP2[edx], ax
			jmp	gp4osplineskip
		gp4osplinesp:
			and	eax, 14
			mov	ax, word ptr GrphPal[eax*2]
			mov	word ptr Grp_LineBufSP[edx], ax
			mov	word ptr Grp_LineBufSP2[edx], 0
		gp4osplineskip:
			add	esi, 2
			add	edx, 2
			dec	bx
			jnz	gp4ospline_cnt
			sub	esi, 0400h
		gp4ospline_cnt:
//			dec	cx
//			jnz	gp4osplinelp
			loop	gp4osplinelp
			pop	esi
		}
/*	}
	else
	{
		__asm
		{
			push	esi
			mov	esi, scry
			add	esi, VLINE
			and	esi, 511
			shl	esi, 10
			mov	ebx, scrx
			and	ebx, 511
			add	esi, ebx
			add	esi, ebx
//			add	esi, vram
			test	page, 2
			jz	gp4splinepage0
			inc	esi
		gp4splinepage0:
			xor	bx, 511
			inc	bx
			mov	ecx, TextDotX
			xor	edx, edx
		gp4splinelp:
			mov	al, byte ptr GVRAM[esi]
			test	al, 1
			jnz	gp4splinesp
			and	eax, 14
			jz	gp4splineskip_2
			mov	ax, word ptr GrphPal[eax*2]
			mov	Grp_LineBuf[edx], ax
		gp4splineskip_2:
			mov	word ptr Grp_LineBufSP[edx], 0
			jmp	gp4splineskip
		gp4splinesp:
			and	eax, 14
			jz	gp4splineskip_2
			mov	ax, word ptr GrphPal[eax*2]
//			mov	Grp_LineBuf[edx], ax
			mov	word ptr Grp_LineBufSP[edx], ax
		gp4splineskip:
			add	esi, 2
			add	edx, 2
			dec	bx
			jnz	gp4spline_cnt
			sub	esi, 0400h
		gp4spline_cnt:
//			dec	cx
//			jnz	gp4splinelp
			loop	gp4splinelp
			pop	esi
		}
	}
*/	}
#elif defined(USE_GAS) && defined(__i386__)
	if (page & 1) {
		__asm__ __volatile__ (
			"pushl	%%esi;"
			"movl	%1, %%esi;"
			"addl	%2, %%esi;"
			"movb	%3, %%al;"
			"andb	$0x1c, %%al;"
			"cmpb	$0x1c, %%al;"
			"jne	.gp4splinenotspecial;"
			"addl	%2, %%esi;"
		".gp4splinenotspecial:"
			"andl	$511, %%esi;"
			"shll	$10, %%esi;"
			"movl	%4, %%ebx;"
			"andl	$511, %%ebx;"
			"addl	%%ebx, %%esi;"
			"addl	%%ebx, %%esi;"
			"testl	$2, %0;"
			"jz	.gp4ospline2page0;"
			"incl	%%esi;"
		".gp4ospline2page0:"
			"xorw	$511, %%bx;"
			"incw	%%bx;"
			"movl	%5, %%ecx;"
			"xorl	%%edx, %%edx;"
		".gp4osplinelp2:"
			"movb	GVRAM(%%esi), %%al;"
			"shrb	$4, %%al;"
			"testb	$1, %%al;"
			"jnz	.gp4o2splinesp;"
			"andl	$14, %%eax;"
			"movw	GrphPal(, %%eax, 2), %%ax;"
			"movw	$0, Grp_LineBufSP(%%edx);"
			"movw	%%ax, Grp_LineBufSP2(%%edx);"
			"jmp	.gp4o2splineskip;"
		".gp4o2splinesp:"
			"andl	$14, %%eax;"
			"movw	GrphPal(, %%eax, 2), %%ax;"
			"movw	%%ax, Grp_LineBufSP(%%edx);"
			"movw	$0, Grp_LineBufSP2(%%edx);"
		".gp4o2splineskip:"
			"addl	$2, %%esi;"
			"addl	$2, %%edx;"
			"decw	%%bx;"
			"jnz	.gp4ospline_cnt2;"
			"subl	$0x0400, %%esi;"
		".gp4ospline_cnt2:"
			"loop	.gp4osplinelp2;"
			"popl	%%esi;"
		: /* output: nothing */
		: "m" (page), "m" (scry), "m" (VLINE), "m" (CRTC_Regs[0x29]),
		  "m" (scrx), "m" (TextDotX)
		: "ax", "bx", "cx", "dx");
	} else {
		__asm__ __volatile__ (
			"pushl	%%esi;"
			"movl	%1, %%esi;"
			"addl	%2, %%esi;"
			"movb	%3, %%al;"
			"andb	$0x1c, %%al;"
			"cmpb	$0x1c, %%al;"
			"jne	.gp4osplinenotspecial;"
			"addl	%2, %%esi;"
		".gp4osplinenotspecial:"
			"andl	$511, %%esi;"
			"shll	$10, %%esi;"
			"movl	%4, %%ebx;"
			"andl	$511, %%ebx;"
			"addl	%%ebx, %%esi;"
			"addl	%%ebx, %%esi;"
			"testl	$2, %0;"
			"jz	.gp4osplinepage0;"
			"incl	%%esi;"
		".gp4osplinepage0:"
			"xorw	$511, %%bx;"
			"incw	%%bx;"
			"movl	%5, %%ecx;"
			"xorl	%%edx, %%edx;"
		".gp4osplinelp:"
			"movb	GVRAM(%%esi), %%al;"
			"testb	$1, %%al;"
			"jnz	.gp4osplinesp;"
			"andl	$14, %%eax;"
			"movw	GrphPal(, %%eax, 2), %%ax;"
			"movw	$0, Grp_LineBufSP(%%edx);"
			"movw	%%ax, Grp_LineBufSP2(%%edx);"
			"jmp	.gp4osplineskip;"
		".gp4osplinesp:"
			"andl	$14, %%eax;"
			"movw	GrphPal(, %%eax, 2), %%ax;"
			"movw	%%ax, Grp_LineBufSP(%%edx);"
			"movw	$0, Grp_LineBufSP2(%%edx);"
		".gp4osplineskip:"
			"addl	$2, %%esi;"
			"addl	$2, %%edx;"
			"decw	%%bx;"
			"jnz	.gp4ospline_cnt;"
			"subl	$0x0400, %%esi;"
		".gp4ospline_cnt:"
			"loop	.gp4osplinelp;"
			"popl	%%esi;"
		: /* output: nothing */
		: "m" (page), "m" (scry), "m" (VLINE), "m" (CRTC_Regs[0x29]),
		  "m" (scrx), "m" (TextDotX)
		: "ax", "bx", "cx", "dx");
	}
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine4SP_C(page, scrx, scry);
#endif /* USE_ASM */
}


void FASTCALL Grp_DrawLine4hSP(void)
{
#ifdef USE_ASM
	__asm
	{
		push	esi
		push	edi
		mov	esi, GrphScrollY[0]
		add	esi, VLINE
		mov	al, CRTC_Regs[0x29]
		and	al, 1ch
		cmp	al, 1ch
		jne	gp4hsplinenotspecial
		add	esi, VLINE
	gp4hsplinenotspecial:
		and	esi, 1023
		test	esi, 512
		jnz	gp4hsp_plane23
		shl	esi, 10
		mov	edx, GrphScrollX[0]
		mov	edi, edx
		and	edx, 511
		add	esi, edx
		add	esi, edx
		mov	cl, 00h
		test	edi, 512
		jz	gp4hsp_main
		add	cl, 4
		jmp	gp4hsp_main
	gp4hsp_plane23:
		and	esi, 511
		shl	esi, 10
		mov	edx, GrphScrollX[0]
		mov	edi, edx
		and	edx, 511
		add	esi, edx
		add	esi, edx
		mov	cl, 08h
		test	edi, 512
		jz	gp4hsp_main
		add	cl, 4
	gp4hsp_main:
		and	edi, 511
		xor	di, 511
		inc	di
		mov	ebx, TextDotX
		xor	edx, edx
	gp4hsplinelp:
		mov	ax, word ptr GVRAM[esi]
		shr	ax, cl
		test	ax, 1
		jnz	gp4hsplinesp
		and	eax, 14
		mov	ax, word ptr GrphPal[eax*2]
		mov	word ptr Grp_LineBufSP[edx], 0
		mov	Grp_LineBufSP2[edx], ax
		jmp	gp4hsplineskip
	gp4hsplinesp:
		and	eax, 14
		mov	ax, word ptr GrphPal[eax*2]
		mov	word ptr Grp_LineBufSP[edx], ax
		mov	word ptr Grp_LineBufSP2[edx], 0
	gp4hsplineskip:
		add	esi, 2
		add	edx, 2
		dec	di
		jnz	gp4hspline_cnt
		sub	esi, 0800h
	gp4hspline_cnt:
		dec	bx
		jnz	gp4hsplinelp
//		loop	gp4hsplinelp
		pop	edi
		pop	esi
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushl	%%esi;"
		"pushl	%%edi;"
		"movl	%0, %%esi;"
		"addl	%1, %%esi;"
		"movb	%2, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp4hsplinenotspecial;"
		"addl	%1, %%esi;"
	".gp4hsplinenotspecial:"
		"andl	$1023, %%esi;"
		"testl	$512, %%esi;"
		"jnz	.gp4hsp_plane23;"
		"shll	$10, %%esi;"
		"movl	%3, %%edx;"
		"movl	%%edx, %%edi;"
		"andl	$511, %%edx;"
		"addl	%%edx, %%esi;"
		"addl	%%edx, %%esi;"
		"movb	$0x00, %%cl;"
		"testl	$512, %%edi;"
		"jz	.gp4hsp_main;"
		"addb	$4, %%cl;"
		"jmp	.gp4hsp_main;"
	".gp4hsp_plane23:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	%3, %%edx;"
		"movl	%%edx, %%edi;"
		"andl	$511, %%edx;"
		"addl	%%edx, %%esi;"
		"addl	%%edx, %%esi;"
		"movb	$0x08, %%cl;"
		"testl	$512, %%edi;"
		"jz	.gp4hsp_main;"
		"addb	$4, %%cl;"
	".gp4hsp_main:"
		"andl	$511, %%edi;"
		"xorw	$511, %%di;"
		"incw	%%di;"
		"movl	%4, %%ebx;"
		"xorl	%%edx, %%edx;"
	".gp4hsplinelp:"
		"movw	GVRAM(%%esi), %%ax;"
		"shrw	%%cl, %%ax;"
		"testw	$1, %%ax;"
		"jnz	.gp4hsplinesp;"
		"andl	$14, %%eax;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	$0, Grp_LineBufSP(%%edx);"
		"movw	%%ax, Grp_LineBufSP2(%%edx);"
		"jmp	.gp4hsplineskip;"
	".gp4hsplinesp:"
		"andl	$14, %%eax;"
		"movw	GrphPal(, %%eax, 2), %%ax;"
		"movw	%%ax, Grp_LineBufSP(%%edx);"
		"movw	$0, Grp_LineBufSP2(%%edx);"
	".gp4hsplineskip:"
		"addl	$2, %%esi;"
		"addl	$2, %%edx;"
		"decw	%%di;"
		"jnz	.gp4hspline_cnt;"
		"subl	$0x0800, %%esi;"
	".gp4hspline_cnt:"
		"decw	%%bx;"
		"jnz	.gp4hsplinelp;"
		"popl	%%edi;"
		"popl	%%esi;"
	: /* output: nothing */
	: "m" (GrphScrollY[0]), "m" (VLINE), "m" (CRTC_Regs[0x29]),
	  "m" (GrphScrollX[0]), "m" (TextDotX)
	: "ax", "bx", "cx", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine4hSP_C();
#endif /* USE_ASM */
}



// -------------------------------------------------
// --- 半透明の対象となるページの描画 --------------
// 2ページ以上あるグラフィックモードのみなので、
// 256色2面 or 16色4面のモードのみ。
// 256色時は、Opaqueでない方のモードはいらないかも…
// （必ずOpaqueモードの筈）
// -------------------------------------------------
// ここはまだ32色x4面モードの実装をしてないれす…
// （れじすた足りないよぅ…）
// -------------------------------------------------
							// やけにすっきり
LABEL void FASTCALL
Grp_DrawLine8TR(int page, int opaq)
{
#ifdef USE_ASM
	__asm {
			or	edx, edx
			jz	opaq_zero
			push	ebx
			push	ecx
			push	esi
			push	edi
			and	ecx, 1
			mov	esi, GrphScrollY[ecx*8]
			add	esi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp8trlinenotspecial
			add	esi, VLINE
		gp8trlinenotspecial:
			and	esi, 511
			shl	esi, 10
			mov	ebx, GrphScrollX[ecx*8]
			and	ebx, 511
			add	esi, ecx
			mov	edx, TextDotX
			xor	edi, edi
		gp8otrlinelp:
			movzx	eax, word ptr Grp_LineBufSP[edi]
			movzx	ecx, byte ptr GVRAM[esi+ebx*2]
			or	ax, ax
			jnz	gp8otrlinetr
			mov	cx, word ptr GrphPal[ecx*2]
			jmp	gp8otrlinenorm
		gp8otrlinetr:
			jcxz	gp8otrlinenorm		// けろぴー…
			mov	cx, word ptr GrphPal[ecx*2]
			jcxz	gp8otrlinenorm		// けろぴー…
			and	ax, Pal_HalfMask
			test	cx, Ibit
			jz	gp8otrlinetrI
			or	ax, Pal_Ix2
		gp8otrlinetrI:
			and	cx, Pal_HalfMask
			add	cx, ax			// 17bit計算中
			rcr	cx, 1			// 17bit計算中
		gp8otrlinenorm:
			mov	Grp_LineBuf[edi], cx
			inc	bx
			and	bh, 1			// and	bx, 511
			add	edi, 2
			dec	dx
			jnz	gp8otrlinelp
			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
		opaq_zero:
			ret
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"movl	%0, %%ecx;"	// ecx = page
		"movl	%1, %%edx;"	// edx = opaq

		"orl	%%edx, %%edx;"
		"jz	.opaq_zero;"

		"andl	$1, %%ecx;"
		"movl	GrphScrollY(, %%ecx, 8), %%esi;"
		"addl	%2, %%esi;"
		"movb	%3, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp8trlinenotspecial;"
		"addl	%2, %%esi;"
	".gp8trlinenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	GrphScrollX(, %%ecx, 8), %%ebx;"
		"andl	$511, %%ebx;"
		"addl	%%ecx, %%esi;"
		"movl	%4, %%edx;"
		"xorl	%%edi, %%edi;"
	".gp8otrlinelp:"
		"movzwl	Grp_LineBufSP(%%edi), %%eax;"
		"movzbl	GVRAM(%%esi, %%ebx, 2), %%ecx;"
		"orw	%%ax, %%ax;"
		"jnz	.gp8otrlinetr;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jmp	.gp8otrlinenorm;"
	".gp8otrlinetr:"
		"jcxz	.gp8otrlinenorm;"	// けろぴー…
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jcxz	.gp8otrlinenorm;"	// けろぴー…
		"andw	%5, %%ax;"
		"testw	%6, %%cx;"
		"jz	.gp8otrlinetrI;"
		"orw	%7, %%ax;"
	".gp8otrlinetrI:"
		"andw	%5, %%cx;"
		"addw	%%ax, %%cx;"		// 17bit計算中
		"rcrw	$1, %%cx;"		// 17bit計算中
	".gp8otrlinenorm:"
		"movw	%%cx, Grp_LineBuf(%%edi);"
		"incw	%%bx;"
		"andb	$1, %%bh;"		// and	bx, 511
		"addl	$2, %%edi;"
		"decw	%%dx;"
		"jnz	.gp8otrlinelp;"

	".opaq_zero:"
	: /* output: nothing */
	: "m" (page), "m" (opaq), "m" (VLINE), "m" (CRTC_Regs[0x29]),
	  "m" (TextDotX), "m" (Pal_HalfMask), "m" (Ibit), "m" (Pal_Ix2)
	: "ax", "bx", "cx", "dx", "si", "di");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine8TR_C(page, opaq);
#endif /* USE_ASM */
}

LABEL void FASTCALL
Grp_DrawLine4TR(DWORD page, int opaq)
{
#ifdef USE_ASM
	__asm {
			push	ebx
			push	ecx
			push	esi
			push	edi

			and	ecx, 3
			mov	esi, GrphScrollY[ecx*4]
			add	esi, VLINE
			mov	al, CRTC_Regs[0x29]
			and	al, 1ch
			cmp	al, 1ch
			jne	gp4trlinenotspecial
			add	esi, VLINE
		gp4trlinenotspecial:
			and	esi, 511
			shl	esi, 10
			mov	ebx, GrphScrollX[ecx*4]
			and	ebx, 511
			xor	edi, edi

			shr	cl, 1
			jnc	pagebit0eq0		// jmp (page 0 or 2)

			add	esi, ecx		// ecx = page/2
			or	edx, edx
			je	gp4trline2page0

			mov	edx, TextDotX
		gp4otrlinelp2:
			movzx	eax, word ptr Grp_LineBufSP[edi]
			movzx	ecx, byte ptr GVRAM[esi+ebx*2]
			shr	cl, 4
			or	ax, ax
			jnz	gp4otrlinetr2
			mov	cx, word ptr GrphPal[ecx*2]
			jmp	gp4otrlinenorm2
		gp4otrlinetr2:
			jcxz	gp4otrlinenorm2		// けろぴー
			mov	cx, word ptr GrphPal[ecx*2]
			jcxz	gp4otrlinenorm2		// けろぴー
			and	ax, Pal_HalfMask
			test	cx, Ibit
			jz	gp4otrlinetr2I
			or	ax, Pal_Ix2
		gp4otrlinetr2I:
			and	cx, Pal_HalfMask
			add	cx, ax			// 17bit計算中
			rcr	cx, 1			// 17bit計算中
		gp4otrlinenorm2:
			mov	Grp_LineBuf[edi], cx
			inc	bx
			and	bh, 1			// and	bx, 511
			add	edi, 2
			dec	dx
			jnz	gp4otrlinelp2
			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
			ret

		gp4trline2page0:
			mov	edx, TextDotX
		gp4trlinelp2:
			movzx	eax, word ptr Grp_LineBufSP[edi]
			or	ax, ax
			jnz	gp4trlinetr2
			movzx	ecx, byte ptr GVRAM[esi+ebx*2]
			shr	cl, 4
			jcxz	gp4trlineskip2
			mov	cx, word ptr GrphPal[ecx*2]
			jmp	gp4trlinenorm2
		gp4trlinetr2:
			movzx	ecx, byte ptr GVRAM[esi+ebx*2]
			shr	cl, 4
			jcxz	gp4trlinenorm2		// けろぴー
			mov	cx, word ptr GrphPal[ecx*2]
			jcxz	gp4trlineskip2		// けろぴー
			and	ax, Pal_HalfMask
			test	cx, Ibit
			jz	gp4trlinetr2I
			or	ax, Pal_Ix2
		gp4trlinetr2I:
			and	cx, Pal_HalfMask
			add	cx, ax			// 17bit計算中
			rcr	cx, 1			// 17bit計算中
		gp4trlinenorm2:
			mov	Grp_LineBuf[edi], cx
		gp4trlineskip2:
			inc	bx
			and	bh, 1			// and	bx, 511
			add	edi, 2
			dec	dx
			jnz	gp4trlinelp2
			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
			ret

		pagebit0eq0:
			add	esi, ecx		// ecx = page/2
			or	edx, edx
			je	gp4trlinepage0

			mov	edx, TextDotX
		gp4otrlinelp:
			mov	cl, byte ptr GVRAM[esi+ebx*2]
			and	ecx, 15
			movzx	eax, word ptr Grp_LineBufSP[edi]
			or	ax, ax
			jnz	gp4otrlinetr
			mov	cx, word ptr GrphPal[ecx*2]
			jmp	gp4otrlinenorm
		gp4otrlinetr:
			jcxz	gp4otrlinenorm		// けろぴー
			mov	cx, word ptr GrphPal[ecx*2]
			jcxz	gp4otrlinenorm		// けろぴー
			and	ax, Pal_HalfMask
			test	cx, Ibit
			jz	gp4otrlinetrI
			or	ax, Pal_Ix2
		gp4otrlinetrI:
			and	cx, Pal_HalfMask
			add	cx, ax			// 17bit計算中
			rcr	cx, 1			// 17bit計算中
		gp4otrlinenorm:
			mov	Grp_LineBuf[edi], cx
			inc	bx
			and	bh, 1			// and	bx, 511
			add	edi, 2
			dec	dx
			jnz	gp4otrlinelp
			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
			ret

		gp4trlinepage0:
			mov	edx, TextDotX
		gp4trlinelp:
			mov	cl, byte ptr GVRAM[esi+ebx*2]
			and	ecx, 15
			movzx	eax, word ptr Grp_LineBufSP[edi]
			or	ax, ax
			jnz	gp4trlinetr

			jcxz	gp4trlineskip
			mov	cx, word ptr GrphPal[ecx*2]
			jmp	gp4trlinenorm

		gp4trlinetr:
			jcxz	gp4trlinenorm		// けろぴー
			mov	cx, word ptr GrphPal[ecx*2]
			jcxz	gp4trlineskip		// けろぴー
			and	ax, Pal_HalfMask
			test	cx, Ibit
			jz	gp4trlinetrI
			or	ax, Pal_Ix2
		gp4trlinetrI:
			and	cx, Pal_HalfMask
			add	cx, ax			// 17bit計算中
			rcr	cx, 1			// 17bit計算中
		gp4trlinenorm:
			mov	Grp_LineBuf[edi], cx
		gp4trlineskip:
			inc	bx
			and	bh, 1			// and	bx, 511
			add	edi, 2
			dec	dx
			jnz	gp4trlinelp
			pop	edi
			pop	esi
			pop	ecx
			pop	ebx
			ret
	}
#elif defined(USE_GAS) && defined(__i386__)
	__asm__ __volatile__ (
		"pushl	%%ebx;"
		"pushl	%%ecx;"
		"pushl	%%esi;"
		"pushl	%%edi;"

		"movl	%0, %%ecx;"	// ecx = page
		"movl	%1, %%edx;"	// edx = opaq

		"andl	$3, %%ecx;"
		"movl	GrphScrollY(, %%ecx, 4), %%esi;"
		"addl	%2, %%esi;"
		"movb	%3, %%al;"
		"andb	$0x1c, %%al;"
		"cmpb	$0x1c, %%al;"
		"jne	.gp4trlinenotspecial;"
		"addl	%2, %%esi;"
	".gp4trlinenotspecial:"
		"andl	$511, %%esi;"
		"shll	$10, %%esi;"
		"movl	GrphScrollX(, %%ecx, 4), %%ebx;"
		"andl	$511, %%ebx;"
		"xorl	%%edi, %%edi;"

		"shrb	$1, %%cl;"
		"jnc	.pagebit0eq0;"

		"addl	%%ecx, %%esi;"
		"orl	%%edx, %%edx;"
		"je	.gp4trline2page0;"

		"movl	%4, %%edx;"
	".gp4otrlinelp2:"
		"movzwl	Grp_LineBufSP(%%edi), %%eax;"
		"movzbl	GVRAM(%%esi, %%ebx, 2), %%ecx;"
		"shrb	$4, %%cl;"
		"orw	%%ax, %%ax;"
		"jnz	.gp4otrlinetr2;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jmp	.gp4otrlinenorm2;"
	".gp4otrlinetr2:"
		"jcxz	.gp4otrlinenorm2;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jcxz	.gp4otrlinenorm2;"
		"andw	%5, %%ax;"
		"testw	%6, %%cx;"
		"jz	.gp4otrlinetr2I;"
		"orw	%7, %%ax;"
	".gp4otrlinetr2I:"
		"andw	%5, %%cx;"
		"addw	%%ax, %%cx;"
		"rcrw	$1, %%cx;"
	".gp4otrlinenorm2:"
		"movw	%%cx, Grp_LineBuf(%%edi);"
		"incw	%%bx;"
		"andb	$1, %%bh;"
		"addl	$2, %%edi;"
		"decw	%%dx;"
		"jnz	.gp4otrlinelp2;"
		"jmp	.gp4tr_end;"

	".gp4trline2page0:"
		"movl	%4, %%edx;"
	".gp4trlinelp2:"
		"movzwl	Grp_LineBufSP(%%edi), %%eax;"
		"orw	%%ax, %%ax;"
		"jnz	.gp4trlinetr2;"
		"movzbl	GVRAM(%%esi, %%ebx, 2), %%ecx;"
		"shrb	$4, %%cl;"
		"jcxz	.gp4trlineskip2;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jmp	.gp4trlinenorm2;"
	".gp4trlinetr2:"
		"movzbl	GVRAM(%%esi, %%ebx, 2), %%ecx;"
		"shrb	$4, %%cl;"
		"jcxz	.gp4trlinenorm2;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jcxz	.gp4trlineskip2;"
		"andw	%5, %%ax;"
		"testw	%6, %%cx;"
		"jz	.gp4trlinetr2I;"
		"orw	%7, %%ax;"
	".gp4trlinetr2I:"
		"andw	%5, %%cx;"
		"addw	%%ax, %%cx;"
		"rcrw	$1, %%cx;"
	".gp4trlinenorm2:"
		"movw	%%cx, Grp_LineBuf(%%edi);"
	".gp4trlineskip2:"
		"incw	%%bx;"
		"andb	$1, %%bh;"
		"addl	$2, %%edi;"
		"decw	%%dx;"
		"jnz	.gp4trlinelp2;"
		"jmp	.gp4tr_end;"

	".pagebit0eq0:"
		"addl	%%ecx, %%esi;"
		"orl	%%edx, %%edx;"
		"je	.gp4trlinepage0;"

		"movl	%4, %%edx;"
	".gp4otrlinelp:"
		"movb	GVRAM(%%esi, %%ebx, 2), %%cl;"
		"andl	$15, %%ecx;"
		"movzx	Grp_LineBufSP(%%edi), %%eax;"
		"orw	%%ax, %%ax;"
		"jnz	.gp4otrlinetr;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jmp	.gp4otrlinenorm;"
	".gp4otrlinetr:"
		"jcxz	.gp4otrlinenorm;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jcxz	.gp4otrlinenorm;"
		"andw	%5, %%ax;"
		"testw	%6, %%cx;"
		"jz	.gp4otrlinetrI;"
		"orw	%7, %%ax;"
	".gp4otrlinetrI:"
		"andw	%5, %%cx;"
		"addw	%%ax, %%cx;"
		"rcrw	$1, %%cx;"
	".gp4otrlinenorm:"
		"movw	%%cx, Grp_LineBuf(%%edi);"
		"incw	%%bx;"
		"andb	$1, %%bh;"
		"addl	$2, %%edi;"
		"decw	%%dx;"
		"jnz	.gp4otrlinelp;"
		"jmp	.gp4tr_end;"

	".gp4trlinepage0:"
		"movl	%4, %%edx;"
	".gp4trlinelp:"
		"movb	GVRAM(%%esi, %%ebx, 2), %%cl;"
		"andl	$15, %%ecx;"
		"movzx	Grp_LineBufSP(%%edi), %%eax;"
		"orw	%%ax, %%ax;"
		"jnz	.gp4trlinetr;"

		"jcxz	.gp4trlineskip;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jmp	.gp4trlinenorm;"

	".gp4trlinetr:"
		"jcxz	.gp4trlinenorm;"
		"movw	GrphPal(, %%ecx, 2), %%cx;"
		"jcxz	.gp4trlineskip;"
		"andw	%5, %%ax;"
		"testw	%6, %%cx;"
		"jz	.gp4trlinetrI;"
		"orw	%7, %%ax;"
	".gp4trlinetrI:"
		"andw	%5, %%cx;"
		"addw	%%ax, %%cx;"
		"rcrw	$1, %%cx;"
	".gp4trlinenorm:"
		"mov	%%cx, Grp_LineBuf(%%edi);"
	".gp4trlineskip:"
		"incw	%%bx;"
		"andb	$1, %%bh;"
		"addl	$2, %%edi;"
		"decw	%%dx;"
		"jnz	.gp4trlinelp;"

	".gp4tr_end:"
		"popl	%%edi;"
		"popl	%%esi;"
		"popl	%%ecx;"
		"popl	%%ebx;"
	: /* output: nothing */
	: "m" (page), "m" (opaq), "m" (VLINE), "m" (CRTC_Regs[0x29]),
	  "m" (TextDotX), "m" (Pal_HalfMask), "m" (Ibit), "m" (Pal_Ix2)
	: "ax", "bx", "cx", "dx");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	Grp_DrawLine4TR_C(page, opaq);
#endif /* USE_ASM */
}

/*
MS-C のを gas のインラインアセンブリにてけとーに変換

s/\<.*:/.&/
s/[^ \t]/"&/
s/[^ \t:]$/&;"/
s/[^ \t]:$/&"/
s/\<e*[abcd][xlh]\>/%%&/g
s/\<e*[ds]i\>/%%&/g
s/\<[0-9]*[^h]\>/$&/g
s/\([^%]\)\(\<0*[0-9a-fA-F]*\)h/\1$0x\2/g
s/"\([^ \t]*\)[ \t][ \t]*\(.*\), \(.*\);/"\1 \3, \2;/
                                            + ここはタブ
s/\[/(/g
s/]/)/g
*/
