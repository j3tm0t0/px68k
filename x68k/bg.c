// ---------------------------------------------------------------------------------------
//  BG.C - BGとスプライト
//  ToDo：透明色の処理チェック（特に対Text間）
// ---------------------------------------------------------------------------------------

#include "common.h"
#include "windraw.h"
#include "winx68k.h"
#include "palette.h"
#include "tvram.h"
#include "crtc.h"
#include "bg.h"
#include "../psp/gecomp.h"

#include "m68000.h"
#include "memory.h"

	BYTE	BG[0x8000] __attribute__ ((aligned (64)));	/* read as DWORDs */
	BYTE	Sprite_Regs[0x800] __attribute__ ((aligned (4)));	/* read as WORDs */
	BYTE	BG_Regs[0x12];
	WORD	BG_CHREND = 0;
	WORD	BG_BG0TOP = 0;
	WORD	BG_BG0END = 0;
	WORD	BG_BG1TOP = 0;
	WORD	BG_BG1END = 0;
	BYTE	BG_CHRSIZE = 16;
	DWORD	BG_AdrMask = 511;
	DWORD	BG0ScrollX = 0, BG0ScrollY = 0;
	DWORD	BG1ScrollX = 0, BG1ScrollY = 0;

	long	BG_HAdjust = 0;
	long	BG_VLINE = 0;

	BYTE	BG_Dirty0[64*64];
	BYTE	BG_Dirty1[64*64];
/*
 * 1 byte per pixel copies of the patterns, only for the x86 assembler
 * drawing code; the C code reads BG[] directly.
 */
#if defined(USE_ASM) || (defined(USE_GAS) && defined(__i386__))
#define	BG_USE_CHRBUF
	BYTE	BGCHR8[8*8*256];
	BYTE	BGCHR16[16*16*256];
#else
/*
 * BG[] with the 16x16 patterns rearranged so that a row is 8 contiguous
 * bytes (pattern p, row r at p * 128 + r * 8): one cache line per row of
 * a tile instead of two.  Kept in sync by BG_Write.
 */
	BYTE	BG16L[0x8000] __attribute__ ((aligned (64)));

/*
 * Sprites by Y position: bit n of Sprite_YBucket[b] is set when
 * (Y of sprite n & 0x3ff) >> 4 == b.  Kept in sync by BG_Write, so that a
 * line only has to look at the sprites of 2 buckets instead of all 128.
 */
static	DWORD	Sprite_YBucket[64][4];

static void
Sprite_ResetYBucket(void)
{
	const WORD *sr = (const WORD *)Sprite_Regs;
	int n;

	memset(Sprite_YBucket, 0, sizeof(Sprite_YBucket));
	for (n = 0; n < 128; n++)
		Sprite_YBucket[(sr[n * 4 + 1] & 0x3ff) >> 4][n >> 5] |= 1u << (n & 31);
}
#endif

	WORD	BG_LineBuf[1600] __attribute__ ((aligned (64)));
	WORD	BG_PriBuf[1600] __attribute__ ((aligned (64)));

	DWORD	VLINEBG = 0;


// -----------------------------------------------------------------------
//   初期化
// -----------------------------------------------------------------------
void BG_Init(void)
{
	DWORD i;
	ZeroMemory(Sprite_Regs, 0x800);
	ZeroMemory(BG, 0x8000);
#ifndef BG_USE_CHRBUF
	ZeroMemory(BG16L, 0x8000);
	Sprite_ResetYBucket();
#else
	ZeroMemory(BGCHR8, 8*8*256);
	ZeroMemory(BGCHR16, 16*16*256);
#endif
	ZeroMemory(BG_LineBuf, 1600*2);
#ifdef PSP
	GE_BGReset();
#endif
	for (i=0; i<0x12; i++)
		BG_Write(0xeb0800+i, 0);
	BG_CHREND = 0x8000;
}


// -----------------------------------------------------------------------
//   I/O Read
// -----------------------------------------------------------------------
BYTE FASTCALL BG_Read(DWORD adr)
{
	if ((adr>=0xeb0000)&&(adr<0xeb0400))
	{
		adr -= 0xeb0000;
		adr ^= 1;
		return Sprite_Regs[adr];
	}
	else if ((adr>=0xeb0800)&&(adr<0xeb0812))
		return BG_Regs[adr-0xeb0800];
	else if ((adr>=0xeb8000)&&(adr<0xec0000))
		return BG[adr-0xeb8000];
	else
		return 0xff;
}


// -----------------------------------------------------------------------
//   I/O Write
// -----------------------------------------------------------------------
void FASTCALL BG_Write(DWORD adr, BYTE data)
{
#ifdef BG_USE_CHRBUF
	DWORD bg16chr;
#endif
	int s1, s2, v = 0;
	s1 = (((BG_Regs[0x11]  &4)?2:1)-((BG_Regs[0x11]  &16)?1:0));
	s2 = (((CRTC_Regs[0x29]&4)?2:1)-((CRTC_Regs[0x29]&16)?1:0));
	if ( !(BG_Regs[0x11]&16) ) v = ((BG_Regs[0x0f]>>s1)-(CRTC_Regs[0x0d]>>s2));
	if ((adr>=0xeb0000)&&(adr<0xeb0400))
	{
		adr &= 0x3ff;
		adr ^= 1;
		if (Sprite_Regs[adr] != data)
		{
			GE_GUARD_SPRITE();
#ifdef USE_ASM
			_asm
			{
				mov	ebx, adr
				and	ebx, 3f8h
				mov	bx, word ptr Sprite_Regs[ebx+2]
				sub	bx, 16
				add	ebx, BG_VLINE
				sub	ebx, v
				and	ebx, 3ffh
				mov	al, 16
			spsetdirtylp1:
				mov	byte ptr TextDirtyLine[ebx], 1
				inc	bx
				and	bx, 3ffh
				dec	al
				jnz	spsetdirtylp1
			}
			Sprite_Regs[adr] = data;
			_asm
			{
				mov	ebx, adr
				and	ebx, 3f8h
				mov	bx, word ptr Sprite_Regs[ebx+2]
				sub	bx, 16
				add	ebx, BG_VLINE
				sub	ebx, v
				and	ebx, 3ffh
				mov	al, 16
			spsetdirtylp2:
				mov	byte ptr TextDirtyLine[ebx], 1
				inc	bx
				and	bx, 3ffh
				dec	al
				jnz	spsetdirtylp2
			}
#elif defined(USE_GAS) && defined(__i386__)
			asm (
				"mov	%0, %%ebx;"
				"and	$0x3f8, %%ebx;"
				"mov	Sprite_Regs + 2(%%ebx), %%bx;"
				"sub	$16, %%bx;"
				"add	(%1), %%ebx;"
				"sub	%2, %%ebx;"
				"and	$0x3ff, %%ebx;"
				"mov	$16, %%al;"
			"0:"
				"movb	$1, TextDirtyLine(%%ebx);"
				"inc	%%bx;"
				"and	$0x3ff, %%bx;"
				"dec	%%al;"
				"jnz	0b;"
			: /* output: nothing */
			: "m" (adr), "g" (BG_VLINE), "m" (v)
			: "ax", "bx", "memory");

			Sprite_Regs[adr] = data;

			asm (
				"mov	%0, %%ebx;"
				"and	$0x3f8, %%ebx;"
				"mov	Sprite_Regs + 2(%%ebx), %%bx;"
				"sub	$16, %%bx;"
				"add	(%1), %%ebx;"
				"sub	%2, %%ebx;"
				"and	$0x3ff, %%ebx;"
				"mov	$16, %%al;"
			"0:"
				"movb	$1, TextDirtyLine(%%ebx);"
				"inc	%%bx;"
				"and	$0x3ff, %%bx;"
				"dec	%%al;"
				"jnz	0b;"
			: /* output: nothing */
			: "m" (adr), "g" (BG_VLINE), "m" (v)
			: "ax", "bx", "memory");
#else /* !USE_ASM && !(USE_GAS && __i386__) */

			WORD t0, t, *pw;

			v = BG_VLINE - 16 - v;
			// get YPOS pointer (Sprite_Regs[] is little endian)
			pw = (WORD *)(Sprite_Regs + (adr & 0x3f8) + 2);

#define UPDATE_TDL(t)				\
{						\
	int i;					\
	for (i = 0; i < 16; i++) {		\
		TextDirtyLine[(t)] = 1;		\
		(t) = ((t) + 1) & 0x3ff;	\
	}					\
}

			t = t0 = (*pw + v) & 0x3ff;
			UPDATE_TDL(t);

			{
				const DWORD ob = (*pw & 0x3ff) >> 4;

				Sprite_Regs[adr] = data;
				if ((adr & 6) == 2) {	/* Y changed */
					const DWORD nb = (*pw & 0x3ff) >> 4;
					const DWORD n = adr >> 3;

					if (nb != ob) {
						Sprite_YBucket[ob][n >> 5] &= ~(1u << (n & 31));
						Sprite_YBucket[nb][n >> 5] |= 1u << (n & 31);
					}
				}
			}

			t = (*pw + v) & 0x3ff;
			if (t != t0) {
				UPDATE_TDL(t);
			}

#endif /* USE_ASM */
		}
	}
	else if ((adr>=0xeb0800)&&(adr<0xeb0812))
	{
		adr -= 0xeb0800;
		if (BG_Regs[adr]==data) return;	// データに変化が無ければ帰る
		BG_Regs[adr] = data;
		switch(adr)
		{
		case 0x00:
		case 0x01:
			BG0ScrollX = (((DWORD)BG_Regs[0x00]<<8)+BG_Regs[0x01])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;
		case 0x02:
		case 0x03:
			BG0ScrollY = (((DWORD)BG_Regs[0x02]<<8)+BG_Regs[0x03])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;
		case 0x04:
		case 0x05:
			BG1ScrollX = (((DWORD)BG_Regs[0x04]<<8)+BG_Regs[0x05])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;
		case 0x06:
		case 0x07:
			BG1ScrollY = (((DWORD)BG_Regs[0x06]<<8)+BG_Regs[0x07])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;

		case 0x08:		// BG On/Off Changed
			TVRAM_SetAllDirty();
			break;

		case 0x0d:
			BG_HAdjust = ((long)BG_Regs[0x0d]-(CRTC_HSTART+4))*8;				// 水平方向は解像度による1/2はいらない？（Tetris）
			TVRAM_SetAllDirty();
			break;
		case 0x0f:
			BG_VLINE = ((long)BG_Regs[0x0f]-CRTC_VSTART)/((BG_Regs[0x11]&4)?1:2);	// BGとその他がずれてる時の差分
			TVRAM_SetAllDirty();
			break;

		case 0x11:		// BG ScreenRes Changed
			if (data&3)
			{
				if ((BG_BG0TOP==0x4000)||(BG_BG1TOP==0x4000))
					BG_CHREND = 0x4000;
				else if ((BG_BG0TOP==0x6000)||(BG_BG1TOP==0x6000))
					BG_CHREND = 0x6000;
				else
					BG_CHREND = 0x8000;
			}
			else
				BG_CHREND = 0x2000;
			BG_CHRSIZE = ((data&3)?16:8);
			BG_AdrMask = ((data&3)?1023:511);
			BG_HAdjust = ((long)BG_Regs[0x0d]-(CRTC_HSTART+4))*8;				// 水平方向は解像度による1/2はいらない？（Tetris）
			BG_VLINE = ((long)BG_Regs[0x0f]-CRTC_VSTART)/((BG_Regs[0x11]&4)?1:2);	// BGとその他がずれてる時の差分
			break;
		case 0x09:		// BG Plane Cfg Changed
			TVRAM_SetAllDirty();
			if (data&0x08)
			{
				if (data&0x30)
				{
					BG_BG1TOP = 0x6000;
					BG_BG1END = 0x8000;
				}
				else
				{
					BG_BG1TOP = 0x4000;
					BG_BG1END = 0x6000;
				}
			}
			else
				BG_BG1TOP = BG_BG1END = 0;
			if (data&0x01)
			{
				if (data&0x06)
				{
					BG_BG0TOP = 0x6000;
					BG_BG0END = 0x8000;
				}
				else
				{
					BG_BG0TOP = 0x4000;
					BG_BG0END = 0x6000;
				}
			}
			else
				BG_BG0TOP = BG_BG0END = 0;
			if (BG_Regs[0x11]&3)
			{
				if ((BG_BG0TOP==0x4000)||(BG_BG1TOP==0x4000))
					BG_CHREND = 0x4000;
				else if ((BG_BG0TOP==0x6000)||(BG_BG1TOP==0x6000))
					BG_CHREND = 0x6000;
				else
					BG_CHREND = 0x8000;
			}
			break;
		case 0x0b:
			break;
		}
		Draw_DrawFlag = 1;

	}
	else if ((adr>=0xeb8000)&&(adr<0xec0000))
	{
		adr -= 0xeb8000;
		if (BG[adr]==data) return;			// データに変化が無ければ帰る
#ifdef PSP
		GE_BGData(adr, data);	/* before BG[] changes */
#endif
		BG[adr] = data;
#ifdef BG_USE_CHRBUF
		if (adr<0x2000)
		{
			BGCHR8[adr*2]   = data>>4;
			BGCHR8[adr*2+1] = data&15;
		}
		bg16chr = ((adr&3)*2)+((adr&0x3c)*4)+((adr&0x40)>>3)+((adr&0x7f80)*2);
		BGCHR16[bg16chr]   = data>>4;
		BGCHR16[bg16chr+1] = data&15;
#else
		BG16L[(adr&0x7f80)|((adr&0x3c)<<1)|((adr&0x40)>>4)|(adr&3)] = data;
#endif

		if (adr<BG_CHREND)				// パターンエリア
		{
			TVRAM_SetAllDirty();
		}
		if ((adr>=BG_BG1TOP)&&(adr<BG_BG1END))	// BG1 MAPエリア
		{
			TVRAM_SetAllDirty();
		}
		if ((adr>=BG_BG0TOP)&&(adr<BG_BG0END))	// BG0 MAPエリア
		{
			TVRAM_SetAllDirty();
		}
	}
}

#ifndef USE_GAS
//#define USE_GAS
#endif

// -----------------------------------------------------------------------
//   1ライン分の描画
// -----------------------------------------------------------------------
#ifdef USE_ASM
#include	"bg.x86"
LABEL void FASTCALL BG_DrawLine(int opaq, int gd) {
	__asm {
			pushf
			push	ebx
			push	esi
			push	edi
			push	edx
			push	ebp

			//xor	eax, eax
			mov	ax, TextPal[0]
			shl	eax, 16
			mov	ax, TextPal[0]
			mov	ebx, 0xffffffff
			mov	edi, 16*2
			or	ecx, ecx			// ecx = opaq
			jz	noclrloop
			mov	ecx, TextDotX
			shr	ecx, 1
		BGLineClr_lp:
			mov	dword ptr BG_LineBuf[edi], eax
			mov	dword ptr BG_PriBuf[edi], ebx	// SP間のプライオリティ情報初期化
			add	edi, 4
			loop	BGLineClr_lp
			jmp	bgclrloopend

		noclrloop:
			mov	ecx, TextDotX
			shr	ecx, 1
		BGLineClr_lp2:
			mov	dword ptr BG_PriBuf[edi], ebx	// SP間のプライオリティ情報初期化
			add	edi, 4
			loop	BGLineClr_lp2

		bgclrloopend:
			or	edx, edx			// edx = gd
			je	BG_NOGRP

			cmp	BG_CHRSIZE, 8
			jne	BG16

			Sprite_DrawLineMcr(81, 1)
;			test	BG_Regs[9], 8
;			je	BG8_1skiped
;			BG_DrawLineMcr8(1, BG_BG1TOP, BG1ScrollX, BG1ScrollY)
;		BG8_1skiped:
			Sprite_DrawLineMcr(82, 2)
			test	BG_Regs[9], 1
			je	BG_0skiped
			BG_DrawLineMcr8(0, BG_BG0TOP, BG0ScrollX, BG0ScrollY)
			jmp	BG_0skiped

		BG16:
			Sprite_DrawLineMcr(161, 1)
;			test	BG_Regs[9], 8
;			je	BG16_1skiped
;			BG_DrawLineMcr16(1, BG_BG1TOP, BG1ScrollX, BG1ScrollY)
;		BG16_1skiped:
			Sprite_DrawLineMcr(162, 2)
			test	BG_Regs[9], 1
			je	BG_0skiped
			BG_DrawLineMcr16(0, BG_BG0TOP, BG0ScrollX, BG0ScrollY)
			jmp	BG_0skiped

		BG_NOGRP:
			cmp	BG_CHRSIZE, 8
			jne	BG16_ng

			Sprite_DrawLineMcr(ng81, 1)
			test	BG_Regs[9], 8
			je	BG8_ng_1skiped
			BG_DrawLineMcr8_ng(1, BG_BG1TOP, BG1ScrollX, BG1ScrollY)
		BG8_ng_1skiped:
			Sprite_DrawLineMcr(ng82, 2)
			test	BG_Regs[9], 1
			je	BG_0skiped
			BG_DrawLineMcr8_ng(0, BG_BG0TOP, BG0ScrollX, BG0ScrollY)
			jmp	BG_0skiped

		BG16_ng:
			Sprite_DrawLineMcr(ng161, 1)
			test	BG_Regs[9], 8
			je	BG16_ng_1skiped
			BG_DrawLineMcr16_ng(1, BG_BG1TOP, BG1ScrollX, BG1ScrollY)
		BG16_ng_1skiped:
			Sprite_DrawLineMcr(ng162, 2)
			test	BG_Regs[9], 1
			je	BG_0skiped
			BG_DrawLineMcr16_ng(0, BG_BG0TOP, BG0ScrollX, BG0ScrollY)
		BG_0skiped:
			Sprite_DrawLineMcr(163, 3)
			pop	ebp
			pop	edx
			pop	edi
			pop	esi
			pop	ebx
			popf
			ret
	}
}
#elif defined(USE_GAS) && defined(__i386__)
#if 0
LABEL void FASTCALL BG_DrawLine(int opaq, int gd) {
	extern LABEL void FASTCALL __BG_DrawLine(int opaq, int gd);
	__BG_DrawLine(opaq, gd);
}
#endif
#else /* !USE_ASM && !(USE_GAS && __i386__) */
/*
 * The patterns are read straight from BG[] (4 bits per pixel, as the
 * 68000 sees it) instead of from the 1-byte-per-pixel copies BGCHR8 and
 * BGCHR16: one aligned 32-bit load fetches 8 pixels, and the working set is
 * half the size.  BG[] is stored in 68000 byte order, so a little-endian load
 * L of a pattern row holds pixel 2k in the high nibble and pixel 2k+1 in the
 * low nibble of byte k.  A row is turned into a "stream" S whose lowest
 * nibble is the next pixel to draw:
 *   left to right: swap the nibbles of each byte of L
 *   right to left: byte-swap L
 *
 * 8x8 pattern p, row r:   BG[p * 32 + r * 4], 4 bytes
 * 16x16 pattern p, row r: BG16L[p * 128 + r * 8] (left 8 pixels),
 *                         BG16L[p * 128 + r * 8 + 4] (right 8 pixels)
 */
typedef DWORD __attribute__((__may_alias__)) DWORD_A;	/* 32-bit access to BYTE/WORD arrays */
#define BG_ROW(off)	(*(const DWORD_A *)(BG + (off)))
#define BG16_ROW(off)	(*(const DWORD_A *)(BG16L + (off)))

static inline DWORD
bg_nibswap(DWORD l)
{
	return ((l >> 4) & 0x0f0f0f0f) | ((l & 0x0f0f0f0f) << 4);
}

static inline DWORD
bg_bswap(DWORD l)
{
	return __builtin_bswap32(l);
}

/*
 * Sprites on the current line, per priority (1-3), in drawing order (127
 * down to 0). Only the sprites of the (at most 2) Y buckets that overlap
 * the line are looked at, with the same tests as before; the registers are
 * read as WORDs (Sprite_Regs holds them in host order: x, y, control,
 * priority in the low byte of the 4th word).
 */
static BYTE Sprite_Line[4][128];
static int Sprite_LineCount[4];

static void
Sprite_CollectLine(void)
{
	const WORD *sr = (const WORD *)Sprite_Regs;
	/* the sprite is on the line if 16 - (y - VLINEBG + BG_VLINE) <= 15 */
	const DWORD ybase = (DWORD)BG_VLINE - VLINEBG - 1;
	const DWORD hadj = (DWORD)BG_HAdjust;
	const DWORD xmax = TextDotX + 16;
	/* y (0-0x3ff) on the line: y + ybase in 0..15, i.e. y in lo..lo+15 */
	const DWORD lo = -ybase, hi = lo + 15;
	DWORD m[4] = { 0, 0, 0, 0 };
	int c1 = 0, c2 = 0, c3 = 0;
	int w;

	if (lo < 0x400) {
		const DWORD *b = Sprite_YBucket[lo >> 4];
		m[0] = b[0]; m[1] = b[1]; m[2] = b[2]; m[3] = b[3];
	}
	if (hi < 0x400 && (hi >> 4) != (lo >> 4)) {
		const DWORD *b = Sprite_YBucket[hi >> 4];
		m[0] |= b[0]; m[1] |= b[1]; m[2] |= b[2]; m[3] |= b[3];
	}

	/* candidates, 127 down to 0 */
	for (w = 3; w >= 0; w--) {
		DWORD mw = m[w];

		while (mw) {
			const int bit = 31 - __builtin_clz(mw);
			const int n = w * 32 + bit;
			const WORD *s = sr + n * 4;
			int pri;

			mw &= ~(1u << bit);
			if ((DWORD)((s[1] & 0x3ff) + ybase) > 15)
				continue;
			pri = s[3] & 3;
			if (!pri)
				continue;	/* priority 0: not displayed */
			if (((s[0] + hadj) & 0x3ff) >= xmax)
				continue;
			switch (pri) {
			case 1: Sprite_Line[1][c1++] = n; break;
			case 2: Sprite_Line[2][c2++] = n; break;
			default: Sprite_Line[3][c3++] = n; break;
			}
		}
	}
	Sprite_LineCount[1] = c1;
	Sprite_LineCount[2] = c2;
	Sprite_LineCount[3] = c3;
}

/* 8 sprite pixels of stream S at lb/tf/pb; pri is the sprite number * 8 */
#define SPRITE_PIX8(S)							\
	do {								\
		DWORD s_ = (S);						\
		int k_ = 0;						\
		for (; s_; s_ >>= 4, k_++) {				\
			DWORD c_ = s_ & 15;				\
			if (c_ && pb[k_] >= pri) {			\
				lb[k_] = pp[c_];			\
				tf[k_] |= 2;				\
				pb[k_] = pri;				\
			}						\
		}							\
	} while (0)

static void
Sprite_DrawLineMcr(int pri_level)
{
	const WORD *sr = (const WORD *)Sprite_Regs;
	const BYTE *list = Sprite_Line[pri_level];
	const int cnt = Sprite_LineCount[pri_level];
	const DWORD ybase = (DWORD)BG_VLINE - VLINEBG;
	const DWORD hadj = (DWORD)BG_HAdjust;
	int k;

	for (k = 0; k < cnt; k++) {
		const int n = list[k];
		const WORD *s = sr + n * 4;
		const DWORD ctrl = s[2];
		const DWORD t = (s[0] + hadj) & 0x3ff;
		/* row of the sprite on this line, 0-15 (checked when collected) */
		DWORD y = (16 - ((s[1] & 0x3ff) + ybase)) & 15;
		const WORD *pp = TextPal + ((ctrl >> 4) & 0xf0);
		WORD *lb = BG_LineBuf + t;
		BYTE *tf = Text_TrFlag + t;
		WORD *pb = BG_PriBuf + t;
		const int pri = n * 8;
		DWORD off, s0, s1;

		if (ctrl & 0x8000)		/* V flip */
			y ^= 15;
		off = (ctrl & 0xff) * 128 + y * 8;
		if (ctrl & 0x4000) {		/* H flip */
			s0 = bg_bswap(BG16_ROW(off + 4));
			s1 = bg_bswap(BG16_ROW(off));
		} else {
			s0 = bg_nibswap(BG16_ROW(off));
			s1 = bg_nibswap(BG16_ROW(off + 4));
		}
		SPRITE_PIX8(s0);
		lb += 8; tf += 8; pb += 8;
		SPRITE_PIX8(s1);
	}
}

/* 8 BG pixels of stream S, only the opaque ones */
#define BG_PIX8_SPARSE(S)						\
	do {								\
		DWORD s_ = (S);						\
		int k_ = 0;						\
		for (; s_; s_ >>= 4, k_++) {				\
			DWORD c_ = s_ & 15;				\
			if (c_) {					\
				lb[k_] = pp[c_];			\
				tf[k_] |= 2;				\
			}						\
		}							\
	} while (0)

/*
 * 8 BG pixels of stream S, palette block != 0 and graphics on: a
 * transparent pixel still draws colour pp[0] where no sprite/BG has been
 * drawn yet.
 */
#define BG_PIX8_FULL(S)							\
	do {								\
		DWORD s_ = (S);						\
		int k_;							\
		for (k_ = 0; k_ < 8; s_ >>= 4, k_++) {			\
			DWORD c_ = s_ & 15;				\
			if (c_ || !(tf[k_] & 2)) {			\
				lb[k_] = pp[c_];			\
				tf[k_] |= 2;				\
			}						\
		}							\
	} while (0)

/*
 * One line of an 8x8 BG plane.  Same pixels as the original
 * bg_drawline_loopx8: (TextDotX >> 3) + 1 tiles starting at
 * BG_LineBuf[16 - (scroll & 7)].
 */
static void
bg_drawline_loopx8(DWORD BGTOP, DWORD BGScrollX, DWORD BGScrollY, long adjust, int gd)
{
	const DWORD sy = BGScrollY + VLINEBG - BG_VLINE;
	const DWORD sx = BGScrollX - adjust;
	const BYTE *map = BG + BGTOP + ((sy & 0x1f8) << 4);
	const DWORD r = sy & 7;
	DWORD col = (sx >> 3) & 63;
	WORD *lb = BG_LineBuf + 16 - (sx & 7);
	BYTE *tf = Text_TrFlag + 16 - (sx & 7);
	int i;

	for (i = TextDotX >> 3; i >= 0; i--) {
		const DWORD bl = map[col * 2];
		const DWORD pat = map[col * 2 + 1];
		const WORD *pp = TextPal + ((bl & 15) << 4);
		const DWORD off = pat * 32 + ((bl & 0x80) ? (7 - r) : r) * 4;
		const DWORD l = BG_ROW(off);
		const DWORD s = (bl & 0x40) ? bg_bswap(l) : bg_nibswap(l);

		if (gd && (bl & 15))
			BG_PIX8_FULL(s);
		else
			BG_PIX8_SPARSE(s);
		lb += 8; tf += 8;
		col = (col + 1) & 63;
	}
}

/*
 * One line of a 16x16 BG plane.  Same pixels as the original
 * bg_drawline_loopx16: (TextDotX >> 4) + 1 tiles starting at
 * BG_LineBuf[16 - (scroll & 15)].
 */
static void
bg_drawline_loopx16(DWORD BGTOP, DWORD BGScrollX, DWORD BGScrollY, long adjust, int gd)
{
	const DWORD sy = BGScrollY + VLINEBG - BG_VLINE;
	const DWORD sx = BGScrollX - adjust;
	const BYTE *map = BG + BGTOP + ((sy & 0x3f0) << 3);
	const DWORD r = sy & 15;
	DWORD col = (sx >> 4) & 63;
	WORD *lb = BG_LineBuf + 16 - (sx & 15);
	BYTE *tf = Text_TrFlag + 16 - (sx & 15);
	int i;

	for (i = TextDotX >> 4; i >= 0; i--) {
		const DWORD bl = map[col * 2];
		const DWORD pat = map[col * 2 + 1];
		const WORD *pp = TextPal + ((bl & 15) << 4);
		const DWORD off = pat * 128 + ((bl & 0x80) ? (15 - r) : r) * 8;
		DWORD s0, s1;

		if (bl & 0x40) {
			s0 = bg_bswap(BG16_ROW(off + 4));
			s1 = bg_bswap(BG16_ROW(off));
		} else {
			s0 = bg_nibswap(BG16_ROW(off));
			s1 = bg_nibswap(BG16_ROW(off + 4));
		}
		if (gd && (bl & 15)) {
			BG_PIX8_FULL(s0);
			lb += 8; tf += 8;
			BG_PIX8_FULL(s1);
		} else {
			BG_PIX8_SPARSE(s0);
			lb += 8; tf += 8;
			BG_PIX8_SPARSE(s1);
		}
		lb += 8; tf += 8;
		col = (col + 1) & 63;
	}
}

LABEL void FASTCALL
BG_DrawLine(int opaq, int gd)
{
	const int cnt = TextDotX;
	DWORD_A *pb = (DWORD_A *)(BG_PriBuf + 16);
	int i;

	if (opaq) {
		const DWORD c = TextPal[0];
		const DWORD c2 = c | (c << 16);
		DWORD_A *lb = (DWORD_A *)(BG_LineBuf + 16);

		for (i = 0; i < (cnt >> 1); i++) {
			lb[i] = c2;
			pb[i] = 0xffffffff;
		}
		if (cnt & 1) {
			BG_LineBuf[16 + cnt - 1] = c;
			BG_PriBuf[16 + cnt - 1] = 0xffff;
		}
	} else {
		for (i = 0; i < (cnt >> 1); i++)
			pb[i] = 0xffffffff;
		if (cnt & 1)
			BG_PriBuf[16 + cnt - 1] = 0xffff;
	}

	Sprite_CollectLine();
	Sprite_DrawLineMcr(1);
	if ((BG_Regs[9] & 8) && (BG_CHRSIZE == 8)) { // BG1 on
		bg_drawline_loopx8(BG_BG1TOP, BG1ScrollX, BG1ScrollY, BG_HAdjust, gd);
	}
	Sprite_DrawLineMcr(2);
	if (BG_Regs[9] & 1) { // BG0 on
		if (BG_CHRSIZE == 8) {
			bg_drawline_loopx8(BG_BG0TOP, BG0ScrollX, BG0ScrollY, BG_HAdjust, gd);
		} else {
			/* the original passed no H adjust for 16x16 without graphics */
			bg_drawline_loopx16(BG_BG0TOP, BG0ScrollX, BG0ScrollY, gd ? BG_HAdjust : 0, gd);
		}
	}
	Sprite_DrawLineMcr(3);
}
#endif /* USE_ASM */
