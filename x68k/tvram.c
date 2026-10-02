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
static const DWORD Text_Expand[256] = {
	0x00000000, 0x10000000, 0x01000000, 0x11000000, 0x00100000, 0x10100000,
	0x01100000, 0x11100000, 0x00010000, 0x10010000, 0x01010000, 0x11010000,
	0x00110000, 0x10110000, 0x01110000, 0x11110000, 0x00001000, 0x10001000,
	0x01001000, 0x11001000, 0x00101000, 0x10101000, 0x01101000, 0x11101000,
	0x00011000, 0x10011000, 0x01011000, 0x11011000, 0x00111000, 0x10111000,
	0x01111000, 0x11111000, 0x00000100, 0x10000100, 0x01000100, 0x11000100,
	0x00100100, 0x10100100, 0x01100100, 0x11100100, 0x00010100, 0x10010100,
	0x01010100, 0x11010100, 0x00110100, 0x10110100, 0x01110100, 0x11110100,
	0x00001100, 0x10001100, 0x01001100, 0x11001100, 0x00101100, 0x10101100,
	0x01101100, 0x11101100, 0x00011100, 0x10011100, 0x01011100, 0x11011100,
	0x00111100, 0x10111100, 0x01111100, 0x11111100, 0x00000010, 0x10000010,
	0x01000010, 0x11000010, 0x00100010, 0x10100010, 0x01100010, 0x11100010,
	0x00010010, 0x10010010, 0x01010010, 0x11010010, 0x00110010, 0x10110010,
	0x01110010, 0x11110010, 0x00001010, 0x10001010, 0x01001010, 0x11001010,
	0x00101010, 0x10101010, 0x01101010, 0x11101010, 0x00011010, 0x10011010,
	0x01011010, 0x11011010, 0x00111010, 0x10111010, 0x01111010, 0x11111010,
	0x00000110, 0x10000110, 0x01000110, 0x11000110, 0x00100110, 0x10100110,
	0x01100110, 0x11100110, 0x00010110, 0x10010110, 0x01010110, 0x11010110,
	0x00110110, 0x10110110, 0x01110110, 0x11110110, 0x00001110, 0x10001110,
	0x01001110, 0x11001110, 0x00101110, 0x10101110, 0x01101110, 0x11101110,
	0x00011110, 0x10011110, 0x01011110, 0x11011110, 0x00111110, 0x10111110,
	0x01111110, 0x11111110, 0x00000001, 0x10000001, 0x01000001, 0x11000001,
	0x00100001, 0x10100001, 0x01100001, 0x11100001, 0x00010001, 0x10010001,
	0x01010001, 0x11010001, 0x00110001, 0x10110001, 0x01110001, 0x11110001,
	0x00001001, 0x10001001, 0x01001001, 0x11001001, 0x00101001, 0x10101001,
	0x01101001, 0x11101001, 0x00011001, 0x10011001, 0x01011001, 0x11011001,
	0x00111001, 0x10111001, 0x01111001, 0x11111001, 0x00000101, 0x10000101,
	0x01000101, 0x11000101, 0x00100101, 0x10100101, 0x01100101, 0x11100101,
	0x00010101, 0x10010101, 0x01010101, 0x11010101, 0x00110101, 0x10110101,
	0x01110101, 0x11110101, 0x00001101, 0x10001101, 0x01001101, 0x11001101,
	0x00101101, 0x10101101, 0x01101101, 0x11101101, 0x00011101, 0x10011101,
	0x01011101, 0x11011101, 0x00111101, 0x10111101, 0x01111101, 0x11111101,
	0x00000011, 0x10000011, 0x01000011, 0x11000011, 0x00100011, 0x10100011,
	0x01100011, 0x11100011, 0x00010011, 0x10010011, 0x01010011, 0x11010011,
	0x00110011, 0x10110011, 0x01110011, 0x11110011, 0x00001011, 0x10001011,
	0x01001011, 0x11001011, 0x00101011, 0x10101011, 0x01101011, 0x11101011,
	0x00011011, 0x10011011, 0x01011011, 0x11011011, 0x00111011, 0x10111011,
	0x01111011, 0x11111011, 0x00000111, 0x10000111, 0x01000111, 0x11000111,
	0x00100111, 0x10100111, 0x01100111, 0x11100111, 0x00010111, 0x10010111,
	0x01010111, 0x11010111, 0x00110111, 0x10110111, 0x01110111, 0x11110111,
	0x00001111, 0x10001111, 0x01001111, 0x11001111, 0x00101111, 0x10101111,
	0x01101111, 0x11101111, 0x00011111, 0x10011111, 0x01011111, 0x11011111,
	0x00111111, 0x10111111, 0x01111111, 0x11111111
};

#ifndef TEXT_USE_DRAWWORK
/*
 * The text screen, 4 bits per pixel: Text_Work4[a] holds the 8 pixels of
 * 68000 TVRAM byte offset a (0-0x1ffff), pixel j in bits 4j..4j+3 (plane 0
 * in the lowest bit).  Kept by TVRAM_Write / TVRAM_RCUpdate; a 256-dot line
 * is 128 bytes, and the 4 planes (which are 128 KB apart, i.e. in the same
 * cache set) are not touched when drawing.
 */
static	DWORD	Text_Work4[0x20000] __attribute__ ((aligned (64)));

static inline void
Text_UpdateWork4(DWORD st)	/* st: TVRAM storage index in plane 0 */
{
	const BYTE *t = TVRAM + st;

	Text_Work4[st ^ 1] = Text_Expand[t[0]] | (Text_Expand[t[0x20000]] << 1)
	    | (Text_Expand[t[0x40000]] << 2) | (Text_Expand[t[0x60000]] << 3);
}
#endif

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
#ifdef TEXT_USE_DRAWWORK
	int i, j;
#endif
	ZeroMemory(TVRAM, 0x80000);
	TVRAM_SetAllDirty();

#ifndef TEXT_USE_DRAWWORK
	ZeroMemory(Text_Work4, sizeof(Text_Work4));
#else
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
#else
	Text_UpdateWork4(adr & 0x1ffff);
#endif	/* USE_ASM */
}


// -----------------------------------------------------------------------
//   �餹�����ԡ����Τ��äפǡ���
// -----------------------------------------------------------------------
void FASTCALL TVRAM_RCUpdate(void)
{
	DWORD adr = ((DWORD)CRTC_Regs[0x2d]<<9);

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
	int i;

	for (i = 0; i < 512; i++)
		Text_UpdateWork4(adr + i);
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
	const DWORD *w;
	const WORD *pal = TextPal;
	WORD *lb = BG_LineBuf + 16;
	BYTE *tf = Text_TrFlag + 16;
	const DWORD dotx = TextDotX;
	DWORD y, x, n, rest, sk, cnt;

	y = TextScrollY + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y &= 0x3ff;

	x = TextScrollX & 0x3ff;
	n = 0x400 - x;			/* the line does not wrap around */
	if (n > dotx)
		n = dotx;

	w = Text_Work4 + (y << 7) + (x >> 3);
	sk = x & 7;
	rest = n;

	if (opaq) {
		const DWORD pal0x2 = pal[0] | ((DWORD)pal[0] << 16);

		/* partial first group */
		if (sk && rest) {
			DWORD s = *w++ >> (sk * 4), k;

			cnt = 8 - sk;
			if (cnt > rest)
				cnt = rest;
			for (k = 0; k < cnt; k++, s >>= 4) {
				DWORD c = s & 15;
				lb[k] = pal[c];
				tf[k] = (c != 0);
			}
			lb += cnt; tf += cnt;
			rest -= cnt;
		}
		for (; rest >= 8; rest -= 8, lb += 8, tf += 8) {
			const DWORD s = *w++;

			if (!s && !((DWORD)(size_t)tf & 3)) {
				/* 8 transparent pixels, 32-bit stores */
				DWORD_A *lw = (DWORD_A *)lb;
				DWORD_A *fw = (DWORD_A *)tf;

				lw[0] = lw[1] = lw[2] = lw[3] = pal0x2;
				fw[0] = fw[1] = 0;
				continue;
			}
#define TEXT_PIX(k)						\
			{					\
				DWORD c = (s >> (k * 4)) & 15;	\
				lb[k] = pal[c];			\
				tf[k] = (c != 0);		\
			}
			TEXT_PIX(0); TEXT_PIX(1); TEXT_PIX(2); TEXT_PIX(3);
			TEXT_PIX(4); TEXT_PIX(5); TEXT_PIX(6); TEXT_PIX(7);
#undef TEXT_PIX
		}
		if (rest) {
			DWORD s = *w, k;

			for (k = 0; k < rest; k++, s >>= 4) {
				DWORD c = s & 15;
				lb[k] = pal[c];
				tf[k] = (c != 0);
			}
			lb += rest; tf += rest;
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
			DWORD s = *w++;

			cnt = 8 - sk;
			if (cnt > rest)
				cnt = rest;
			if (s) {
				int k;

				s >>= sk * 4;
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
		}
	}
#endif	/* USE_ASM */
}
