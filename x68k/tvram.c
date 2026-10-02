// ---------------------------------------------------------------------------------------
//  TVRAM.C - Text VRAM
//  ToDo : Ʃ���������Ȥ�����
// ---------------------------------------------------------------------------------------

#include	"common.h"
#include	"winx68k.h"
#include	"windraw.h"
#include	"bg.h"
#include	"crtc.h"
#include	"palette.h"
#include	"m68000.h"
#include	"tvram.h"

	BYTE	TVRAM[0x80000];
	BYTE	TextDirtyLine[1024];

/*
 * 1 byte per pixel copy of the text screen, only for the x86 assembler
 * drawing code; the C Text_DrawLine decodes the planes itself.
 */
#if defined(USE_ASM) || (defined(USE_GAS) && defined(__i386__))
#define	TEXT_USE_DRAWWORK
	BYTE	TextDrawWork[1024*1024];
	BYTE	TextDrawPattern[2048*4];
#endif

/* bit 7-j of a plane byte -> bit 0 of nibble j (pixel j) */
static	DWORD	Text_Expand[256];

typedef DWORD __attribute__((__may_alias__)) DWORD_A;	/* 32-bit access to BYTE/WORD arrays */

//	WORD	Text_LineBuf[1024];	// ��BG�Τ�Ȥ��褦���ѹ�
	BYTE	Text_TrFlag[1024] __attribute__ ((aligned (64)));

INLINE void TVRAM_WriteByteMask(DWORD adr, BYTE data);

// -----------------------------------------------------------------------
//   �����񤭴�����
// -----------------------------------------------------------------------
void TVRAM_SetAllDirty(void)
{
	memset(TextDirtyLine, 1, 1024);
}


// -----------------------------------------------------------------------
//   �����
// -----------------------------------------------------------------------
void TVRAM_Init(void)
{
	int i, j;
	ZeroMemory(TVRAM, 0x80000);
	TVRAM_SetAllDirty();

	for (i=0; i<256; i++)
	{
		Text_Expand[i] = 0;
		for (j=0; j<8; j++)
			if (i & (0x80>>j))
				Text_Expand[i] |= 1u << (j*4);
	}
#ifdef TEXT_USE_DRAWWORK
	ZeroMemory(TextDrawWork, 1024*1024);
	ZeroMemory(TextDrawPattern, 2048*4);		// �ѥ�����ơ��֥�����
	for (i=0; i<256; i++)
	{
		int bit;
		for (j=0, bit=0x80; j<8; j++, bit>>=1)
		{
			if (i&bit) {
				TextDrawPattern[i*8+j     ] = 1;
				TextDrawPattern[i*8+j+2048] = 2;
				TextDrawPattern[i*8+j+4096] = 4;
				TextDrawPattern[i*8+j+6144] = 8;
			}
		}
	}
#endif
}


// -----------------------------------------------------------------------
//   ű��
// -----------------------------------------------------------------------
void TVRAM_Cleanup(void)
{
}


// -----------------------------------------------------------------------
//   �ɤ�ʤ�
// -----------------------------------------------------------------------
BYTE FASTCALL TVRAM_Read(DWORD adr)
{
	adr &= 0x7ffff;
	adr ^= 1;
	return TVRAM[adr];
}


// -----------------------------------------------------------------------
//   1�Ф��Ƚ񤯤ʤ�
// -----------------------------------------------------------------------
INLINE void TVRAM_WriteByte(DWORD adr, BYTE data)
{
	if (TVRAM[adr]!=data)
	{
		TextDirtyLine[(((adr&0x1ffff)/128)-TextScrollY)&1023] = 1;
		TVRAM[adr] = data;
	}
}


// -----------------------------------------------------------------------
//   �ޤ����դ��ǽ񤯤ʤ�
// -----------------------------------------------------------------------
INLINE void TVRAM_WriteByteMask(DWORD adr, BYTE data)
{
	data = (TVRAM[adr] & CRTC_Regs[0x2e + ((adr^1) & 1)]) | (data & (~CRTC_Regs[0x2e + ((adr ^ 1) & 1)]));
	if (TVRAM[adr] != data)
	{
		TextDirtyLine[(((adr&0x1ffff)/128)-TextScrollY)&1023] = 1;
		TVRAM[adr] = data;
	}
}


// -----------------------------------------------------------------------
//   �񤯤ʤ�
// -----------------------------------------------------------------------
void FASTCALL TVRAM_Write(DWORD adr, BYTE data)
{
	adr &= 0x7ffff;
	adr ^= 1;
	if (CRTC_Regs[0x2a]&1)			// Ʊ����������
	{
		adr &= 0x1ffff;
		if (CRTC_Regs[0x2a]&2)		// Text Mask
		{
			if (CRTC_Regs[0x2b]&0x10) TVRAM_WriteByteMask(adr        , data);
			if (CRTC_Regs[0x2b]&0x20) TVRAM_WriteByteMask(adr+0x20000, data);
			if (CRTC_Regs[0x2b]&0x40) TVRAM_WriteByteMask(adr+0x40000, data);
			if (CRTC_Regs[0x2b]&0x80) TVRAM_WriteByteMask(adr+0x60000, data);
		}
		else
		{
			if (CRTC_Regs[0x2b]&0x10) TVRAM_WriteByte(adr        , data);
			if (CRTC_Regs[0x2b]&0x20) TVRAM_WriteByte(adr+0x20000, data);
			if (CRTC_Regs[0x2b]&0x40) TVRAM_WriteByte(adr+0x40000, data);
			if (CRTC_Regs[0x2b]&0x80) TVRAM_WriteByte(adr+0x60000, data);
		}
	}
	else					// ���󥰥륢������
	{
		if (CRTC_Regs[0x2a]&2)		// Text Mask
		{
			TVRAM_WriteByteMask(adr, data);
		}
		else
		{
			TVRAM_WriteByte(adr, data);
		}
	}
#ifdef USE_ASM
	_asm {
		push	edi
		push	esi

		mov	eax, adr
		mov	esi, eax
		and	esi, 01ffffh		; TVRAM Adr
		mov	edi, eax
		and	edi, 01ff80h		; ����7bit�ޥ���
		shl	edi, 3
		and	eax, 07fh
		xor	al, 1
		shl	eax, 3
		add	edi, eax		; edi = workadr

		xor	eax, eax

		mov	al, byte ptr TVRAM[esi+60000h]
		mov	ecx, dword ptr (TextDrawPattern+6144)[eax*8]
		mov	edx, dword ptr (TextDrawPattern+6144)[eax*8+4]
		mov	al, byte ptr TVRAM[esi+40000h]
		or	ecx, dword ptr (TextDrawPattern+4096)[eax*8]
		or	edx, dword ptr (TextDrawPattern+4096)[eax*8+4]
		mov	al, byte ptr TVRAM[esi+20000h]
		or	ecx, dword ptr (TextDrawPattern+2048)[eax*8]
		or	edx, dword ptr (TextDrawPattern+2048)[eax*8+4]
		mov	al, byte ptr TVRAM[esi]
		or	ecx, dword ptr TextDrawPattern[eax*8]
		or	edx, dword ptr TextDrawPattern[eax*8+4]
		mov	dword ptr TextDrawWork[edi], ecx
		mov	dword ptr (TextDrawWork+4)[edi], edx

		pop	esi
		pop	edi
	}
#elif defined(USE_GAS) && defined(__i386__)
	asm (
		"mov	%0, %%eax;"
		"mov	%%eax, %%esi;"
		"and	$0x1ffff, %%esi;"	/* TVRAM Adr */
		"mov	%%eax, %%edi;"
		"and	$0x1ff80, %%edi;"	/* ����7bit�ޥ��� */
		"shl	$3, %%edi;"
		"and	$0x7f, %%eax;"
		"xor	$1, %%al;"
		"shl	$3, %%eax;"
		"add	%%eax, %%edi;"		/* edi = workadr */

		"xor	%%eax, %%eax;"

		"mov	TVRAM + 0x60000(%%esi), %%al;"
		"mov	TextDrawPattern + 6144(, %%eax, 8), %%ecx;"
		"mov	TextDrawPattern + 6144 + 4(, %%eax, 8), %%edx;"
		"mov	TVRAM + 0x40000(%%esi), %%al;"
		"or	TextDrawPattern + 4096(, %%eax, 8), %%ecx;"
		"or	TextDrawPattern + 4096 + 4(, %%eax, 8), %%edx;"
		"mov	TVRAM + 0x20000(%%esi), %%al;"
		"or	TextDrawPattern + 2048(, %%eax, 8), %%ecx;"
		"or	TextDrawPattern + 2048 + 4(, %%eax, 8), %%edx;"
		"mov	TVRAM(%%esi), %%al;"
		"or	TextDrawPattern(, %%eax, 8), %%ecx;"
		"or	TextDrawPattern + 4(, %%eax, 8), %%edx;"
		"mov	%%ecx, TextDrawWork(%%edi);"
		"mov	%%edx, TextDrawWork + 4(%%edi);"
	: /* output: nothing */
	: "m" (adr)
	: "ax", "cx", "dx", "si", "di", "memory");
#endif	/* USE_ASM */
}


// -----------------------------------------------------------------------
//   �餹�����ԡ����Τ��äפǡ���
// -----------------------------------------------------------------------
void FASTCALL TVRAM_RCUpdate(void)
{
#ifdef TEXT_USE_DRAWWORK
	DWORD adr = ((DWORD)CRTC_Regs[0x2d]<<9);
#endif

#ifdef USE_ASM
	_asm
	{
		push	edi
		push	esi
		mov	esi, adr
		mov	edi, esi
		shl	edi, 3
		mov	esi, adr
		mov	cx, 512
		xor	eax, eax
	rcu_mainloop:
		xor	esi, 1
		mov	al, byte ptr TVRAM[esi+60000h]
		mov	ebx, dword ptr (TextDrawPattern+6144)[eax*8]
		mov	edx, dword ptr (TextDrawPattern+6144)[eax*8+4]
		mov	al, byte ptr TVRAM[esi+40000h]
		or	ebx, dword ptr (TextDrawPattern+4096)[eax*8]
		or	edx, dword ptr (TextDrawPattern+4096)[eax*8+4]
		mov	al, byte ptr TVRAM[esi+20000h]
		or	ebx, dword ptr (TextDrawPattern+2048)[eax*8]
		or	edx, dword ptr (TextDrawPattern+2048)[eax*8+4]
		mov	al, byte ptr TVRAM[esi]
		or	ebx, dword ptr TextDrawPattern[eax*8]
		or	edx, dword ptr TextDrawPattern[eax*8+4]
		mov	dword ptr TextDrawWork[edi], ebx
		add	edi, 4
		mov	dword ptr TextDrawWork[edi], edx
		add	edi, 4
		xor	esi, 1
		inc	esi
		dec	cx
		jnz	rcu_mainloop
//		loop	rcu_mainloop
		pop	esi
		pop	edi
	}
#elif defined(USE_GAS) && defined(__i386__)
	asm (
		"mov	%0, %%esi;"
		"mov	%%esi, %%edi;"
		"shl	$3, %%edi;"
		"mov	%0, %%esi;"
		"mov	$512, %%cx;"
		"xor	%%eax, %%eax;"
	".rcu_mainloop:"
		"xor	$1, %%esi;"
		"mov	TVRAM + 0x60000(%%esi), %%al;"
		"mov	TextDrawPattern + 6144(, %%eax, 8), %%ebx;"
		"mov	TextDrawPattern + 6144 + 4(, %%eax, 8), %%edx;"
		"mov	TVRAM + 0x40000(%%esi), %%al;"
		"or	TextDrawPattern + 4096(, %%eax, 8), %%ebx;"
		"or	TextDrawPattern + 4096 + 4(, %%eax, 8), %%edx;"
		"mov	TVRAM + 0x20000(%%esi), %%al;"
		"or	TextDrawPattern + 2048(, %%eax, 8), %%ebx;"
		"or	TextDrawPattern + 2048 + 4(, %%eax, 8), %%edx;"
		"mov	TVRAM(%%esi), %%al;"
		"or	TextDrawPattern(, %%eax, 8), %%ebx;"
		"or	TextDrawPattern + 4(, %%eax, 8), %%edx;"
		"mov	%%ebx, TextDrawWork(%%edi);"
		"add	$4, %%edi;"
		"mov	%%edx, TextDrawWork(%%edi);"
		"add	$4, %%edi;"
		"xor	$1, %%esi;"
		"inc	%%esi;"
		"loop	.rcu_mainloop;"
	: /* output: nothing */
	: "m" (adr)
	: "ax", "bx", "cx", "dx", "si", "di", "memory");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	/* nothing to do: Text_DrawLine reads TVRAM directly */
#endif	/* USE_ASM */
}

// -----------------------------------------------------------------------
//   1�饤������
// -----------------------------------------------------------------------
void FASTCALL Text_DrawLine(int opaq)
{
#ifdef USE_ASM
	__asm {
		push	edi
		or	ecx, ecx		//ecx = opaq
		jz	tdlnotopaq
		mov	edi, 0
		mov	edx, VLINE
		mov	al, CRTC_Regs[0x29]
		and	al, 1ch
		cmp	al, 1ch
		jne	textlinenotspecial
		shl	edx, 1
	textlinenotspecial:
		add	edx, TextScrollY
		and	edx, 1023
		shl	edx, 10
		mov	ebx, TextScrollX
		and	ebx, 1023
		add	edx, ebx
		xor	bx, 1023
		inc	bx
		mov	ecx, TextDotX
	looptextline:
		movzx	eax, byte ptr TextDrawWork[edx]
		mov	byte ptr (Text_TrFlag+16)[edi], 0
		and	al, 15
		jz	textline_skip
		mov	byte ptr (Text_TrFlag+16)[edi], 1
	textline_skip:
		mov	ax, word ptr TextPal[eax*2]
		mov	word ptr (BG_LineBuf+32)[edi*2], ax
		inc	edi
		inc	edx
		dec	bx
		jz	endtextline
		loop	looptextline
		jmp	finishtextline
	endtextline:
		dec	cx
		jz	finishtextline
		mov	ax, word ptr TextPal[0]
	endtextlineloop:
		mov	word ptr (BG_LineBuf+32)[edi*2], ax
		mov	byte ptr (Text_TrFlag+16)[edi], 0
		inc	edi
		loop	endtextlineloop
		jmp	finishtextline


	tdlnotopaq:
		mov	edi, 0
		mov	edx, VLINE
		mov	al, CRTC_Regs[0x29]
		and	al, 1ch
		cmp	al, 1ch
		jne	notextlinenotspecial
		shl	edx, 1
	notextlinenotspecial:
		add	edx, TextScrollY
		and	edx, 1023
		shl	edx, 10
		mov	ebx, TextScrollX
		and	ebx, 1023
		add	edx, ebx
		xor	bx, 1023
		inc	bx
		mov	ecx, TextDotX
	nolooptextline:
		movzx	eax, byte ptr TextDrawWork[edx]
		and	al, 15
		jz	notextline_skip
		or	byte ptr (Text_TrFlag+16)[edi], 1
		mov	ax, word ptr TextPal[eax*2]
		mov	word ptr (BG_LineBuf+32)[edi*2], ax
	notextline_skip:
		inc	edi
		inc	edx
		dec	bx
		jz	finishtextline
		loop	nolooptextline

	finishtextline:
		pop	edi
	}
#elif defined(USE_GAS) && defined(__i386__)
	asm (
		"mov	%0, %%ecx;"
		"or	%%ecx, %%ecx;"
		"jz	.tdlnotopaq;"
		"mov	$0, %%edi;"
		"mov	(%1), %%edx;"
		"mov	(%2), %%al;"
		"and	$0x1c, %%al;"
		"cmp	$0x1c, %%al;"
		"jne	.textlinenotspecial;"
		"shl	$1, %%edx;"
	".textlinenotspecial:"
		"add	(%3), %%edx;"
		"and	$1023, %%edx;"
		"shl	$10, %%edx;"
		"mov	(%4), %%ebx;"
		"and	$1023, %%ebx;"
		"add	%%ebx, %%edx;"
		"xor	$1023, %%bx;"
		"inc	%%bx;"
		"mov	(%6), %%ecx;"
	".looptextline:"
		"movzbl	TextDrawWork(%%edx), %%eax;"
		"movb	$0, Text_TrFlag + 16(%%edi);"
		"and	$15, %%al;"
		"jz	.textline_skip;"
		"movb	$1, Text_TrFlag + 16(%%edi);"
	".textline_skip:"
		"mov	TextPal(, %%eax, 2), %%ax;"
		"mov	%%ax, BG_LineBuf + 32(, %%edi, 2);"
		"inc	%%edi;"
		"inc	%%edx;"
		"dec	%%bx;"
		"jz	.endtextline;"
		"loop	.looptextline;"
		"jmp	.finishtextline;"
	".endtextline:"
		"dec	%%cx;"
		"jz	.finishtextline;"
		"mov	(%5), %%ax;"
	".endtextlineloop:"
		"mov	%%ax, BG_LineBuf + 32(, %%edi, 2);"
		"movb	$0, Text_TrFlag + 16(%%edi);"
		"inc	%%edi;"
		"loop	.endtextlineloop;"
		"jmp	.finishtextline;"

	".tdlnotopaq:"
		"mov	$0, %%edi;"
		"mov	(%1), %%edx;"
		"mov	(%2), %%al;"
		"and	$0x1c, %%al;"
		"cmp	$0x1c, %%al;"
		"jne	.notextlinenotspecial;"
		"shl	$1, %%edx;"
	".notextlinenotspecial:"
		"add	(%3), %%edx;"
		"and	$1023, %%edx;"
		"shl	$10, %%edx;"
		"mov	(%4), %%ebx;"
		"and	$1023, %%ebx;"
		"add	%%ebx, %%edx;"
		"xor	$1023, %%bx;"
		"inc	%%bx;"
		"mov	(%6), %%ecx;"
	".nolooptextline:"
		"movzbl	TextDrawWork(%%edx), %%eax;"
		"and	$15, %%al;"
		"jz	.notextline_skip;"
		"orb	$1, Text_TrFlag + 16(%%edi);"
		"mov	TextPal(, %%eax, 2), %%ax;"
		"mov	%%ax, BG_LineBuf + 32(, %%edi, 2);"
	".notextline_skip:"
		"inc	%%edi;"
		"inc	%%edx;"
		"dec	%%bx;"
		"jz	.finishtextline;"
		"loop	.nolooptextline;"

	".finishtextline:"
	: /* output: nothing */
	: "m" (opaq), "g" (VLINE), "g" (CRTC_Regs[0x29]),
	  "g" (TextScrollY), "g" (TextScrollX), "g" (TextPal[0]), "g" (TextDotX)
	: "ax", "bx", "cx", "dx", "si", "di", "memory");
#else /* !USE_ASM && !(USE_GAS && __i386__) */
	/*
	 * Decode straight from the 4 TVRAM planes, 8 pixels (one byte of each
	 * plane) at a time; an all-zero group of a transparent line is
	 * skipped.  TVRAM is stored in 68000 order with the bytes of each word
	 * swapped; TextDrawWork (which held the same pixels 1 byte each) is
	 * no longer needed.
	 */
	const DWORD *ex = Text_Expand;
	const WORD *pal = TextPal;
	WORD *lb = BG_LineBuf + 16;
	BYTE *tf = Text_TrFlag + 16;
	const DWORD dotx = TextDotX;
	DWORD y, x, n, rest, sk, a;

	y = TextScrollY + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y &= 0x3ff;

	x = TextScrollX & 0x3ff;
	n = 0x400 - x;			/* the line does not wrap around */
	if (n > dotx)
		n = dotx;

	a = (y << 7) + (x >> 3);	/* 68000 address of the first byte */
	sk = x & 7;
	rest = n;

	if (opaq) {
		const DWORD pal0x2 = pal[0] | ((DWORD)pal[0] << 16);

		while (rest) {
			const BYTE *t = TVRAM + (a ^ 1);
			DWORD cnt = 8 - sk;
			DWORD s, k;

			if (cnt > rest)
				cnt = rest;
			if (cnt == 8 && !((DWORD)(size_t)tf & 3)
			    && !(t[0] | t[0x20000] | t[0x40000] | t[0x60000])) {
				/* 8 transparent pixels, 32-bit stores */
				DWORD_A *lw = (DWORD_A *)lb;
				DWORD_A *fw = (DWORD_A *)tf;

				lw[0] = lw[1] = lw[2] = lw[3] = pal0x2;
				fw[0] = fw[1] = 0;
				lb += 8; tf += 8;
				rest -= 8;
				a++;
				continue;
			}
			s = (ex[t[0]] | (ex[t[0x20000]] << 1)
			    | (ex[t[0x40000]] << 2) | (ex[t[0x60000]] << 3))
			    >> (sk * 4);
			if (cnt == 8) {
#define TEXT_PIX(k)						\
				{					\
					DWORD c = (s >> (k * 4)) & 15;	\
					lb[k] = pal[c];			\
					tf[k] = (c != 0);		\
				}
				TEXT_PIX(0); TEXT_PIX(1); TEXT_PIX(2); TEXT_PIX(3);
				TEXT_PIX(4); TEXT_PIX(5); TEXT_PIX(6); TEXT_PIX(7);
#undef TEXT_PIX
			} else {
				for (k = 0; k < cnt; k++, s >>= 4) {
					DWORD c = s & 15;
					lb[k] = pal[c];
					tf[k] = (c != 0);
				}
			}
			lb += cnt; tf += cnt;
			rest -= cnt;
			sk = 0;
			a++;
		}
		if (n != dotx) {
			/* (sic) the original fills one pixel less here */
			DWORD i;
			const WORD c = pal[0];

			for (i = n + 1; i < dotx; i++) {
				*lb++ = c;
				*tf++ = 0;
			}
		}
	} else {
		while (rest) {
			const BYTE *t = TVRAM + (a ^ 1);
			DWORD cnt = 8 - sk;

			if (cnt > rest)
				cnt = rest;
			if (t[0] | t[0x20000] | t[0x40000] | t[0x60000]) {
				DWORD s = (ex[t[0]] | (ex[t[0x20000]] << 1)
				    | (ex[t[0x40000]] << 2) | (ex[t[0x60000]] << 3))
				    >> (sk * 4);
				int k;

				if (cnt < 8)
					s &= (1u << (cnt * 4)) - 1;
				for (k = 0; s; s >>= 4, k++) {
					DWORD c = s & 15;
					if (c) {
						tf[k] |= 1;
						lb[k] = pal[c];
					}
				}
			}
			lb += cnt; tf += cnt;
			rest -= cnt;
			sk = 0;
			a++;
		}
	}
#endif	/* USE_ASM */
}
