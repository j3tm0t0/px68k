#include <string.h>
#include <stddef.h>
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
#define FASTCALL
#define LABEL
#define ZeroMemory(d,n) memset(d,0,n)

#define PAD 0x4000
extern BYTE gv_store[PAD + 0x80000 + PAD];
extern WORD pal_store[65536 + 1024];
#define GVRAM (gv_store + PAD)
#define GrphPal pal_store

extern WORD Grp_LineBuf[1024], Grp_LineBufSP[1024], Grp_LineBufSP2[1024];
extern WORD Pal16Adr[256];
extern BYTE Pal_Regs[1024];
extern WORD Pal16[65536];
extern WORD Ibit, Pal_HalfMask, Pal_Ix2;
extern BYTE CRTC_Regs[48];
extern DWORD TextDotX, TextDotY;
extern DWORD GrphScrollX[4], GrphScrollY[4];
extern DWORD VLINE;
extern BYTE TextDirtyLine[1024];
extern WORD CRTC_FastClrMask;
