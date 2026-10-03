typedef unsigned char BYTE; typedef unsigned short WORD; typedef unsigned int DWORD;
BYTE pad_before[0x4000];
BYTE Pal_Regs[1024];
WORD GrphPal[256 + 65536 + 1024] __attribute__((aligned(4)));
WORD Pal16[65536];
WORD Ibit, Pal_HalfMask, Pal_Ix2;
BYTE CRTC_Regs[48];
DWORD TextDotX, TextDotY;
DWORD GrphScrollX[4], GrphScrollY[4];
DWORD VLINE;
BYTE TextDirtyLine[1024];
WORD CRTC_FastClrMask;
BYTE VCReg0[2], VCReg1[2], VCReg2[2];
void *memset(void *d, int c, unsigned n) { BYTE *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; return d; }
void _start(void) { }
