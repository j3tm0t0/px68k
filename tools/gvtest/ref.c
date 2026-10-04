/* host side of the PSP-binary check: the original decoders' globals */
#include "stub.h"

BYTE gv_store[PAD + 0x80000 + PAD] __attribute__((aligned(64)));
WORD pal_store[65536 + 1024] __attribute__((aligned(64)));
WORD Grp_LineBuf[1024] __attribute__((aligned(64)));
WORD Grp_LineBufSP[1024] __attribute__((aligned(64)));
WORD Grp_LineBufSP2[1024] __attribute__((aligned(64)));
WORD Pal16Adr[256];
BYTE Pal_Regs[1024];
WORD Pal16[65536];
WORD Ibit, Pal_HalfMask, Pal_Ix2;
BYTE CRTC_Regs[48];
DWORD TextDotX, TextDotY;
DWORD GrphScrollX[4], GrphScrollY[4];
DWORD VLINE;
BYTE TextDirtyLine[1024];
WORD CRTC_FastClrMask;
