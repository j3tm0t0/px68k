#ifndef PX68K_PSP_GECOMP_H
#define PX68K_PSP_GECOMP_H

/*
 * GE compositing (PSP): the lines of the modes the GE can draw exactly like
 * the CPU path (x11/windraw.c DrawLine) are not composited by the CPU; their
 * register state is recorded per line, runs of lines with the same state
 * become bands, and the bands are drawn by the GE into the screen texture
 * (ScrBufL) from GVRAM, the text screen (TextDrawWork) and the BG patterns.
 *
 * The bands are drawn when the frame is shown (or earlier, see below), but
 * must look as the CPU path would have drawn them line by line.  So writes
 * to what they read call a guard first (GE_GUARD_*): if the write hits
 * memory a waiting band (or the GE, still drawing) reads, the waiting
 * bands are drawn and the GE is waited for before the write; sprite
 * registers are copied for the waiting bands instead, and the GE's BG
 * pattern copies are updated once it is done.  Palettes and registers are
 * copied per band.
 */

#ifdef PSP

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int GE_Enabled;		/* "ge on|off" */
extern int GE_TimeSync;		/* "ge time": wait for the GE after each frame, to time it */
extern volatile int GE_Guard;	/* bands are waiting for the GE, or the GE is drawing */
extern int GE_PalDirty;		/* set by TVRAM_SetAllDirty: palettes/registers may have changed */

/* windraw.c: draw the waiting bands now and wait until the GE is done */
void WinDraw_GESync(void);
/* windraw.c: wait until the GE is done (with what it was given) */
void WinDraw_GEWait(void);
/* windraw.c: hand the waiting bands to the GE without waiting (if GE_CanKick) */
void WinDraw_GEKick(void);
/* windraw.c: GE_Done() if the GE is done, without waiting */
void WinDraw_GEPoll(void);

void GE_GvramGuard(DWORD adr);	/* GVRAM_Write, its address */
void GE_TvramGuard(DWORD adr);	/* TVRAM_Write, its address */
void GE_SpriteGuard(void);	/* a sprite register changes */
void GE_FullGuard(int why);	/* anything else: wait (why: GE_ST_*) */

/*
 * Generations of the GVRAM rows (16 colour 512 dot layout) and TextDrawWork
 * rows, bumped when a write changes them (the GE's copies of them in VRAM
 * are only refreshed when they changed); *GenAll: all rows.  Every change of
 * GVRAM / TextDrawWork must bump them: they are only written by GVRAM_Write,
 * GVRAM_FastClear, GVRAM_Init, TVRAM_Write, TVRAM_RCUpdate and TVRAM_Init.
 */
extern DWORD GE_GRowGen[512], GE_TRowGen[1024], GE_GGenAll, GE_TGenAll;
/*
 * The contract for anything that writes GVRAM: after the GVRAM word at byte
 * offset a (GVRAM + (a & ~1), 0-0x7ffff) has changed, in any layout, call
 * GE_GVRAM_ROW(a) once (once per word is enough; not needed when the value
 * did not change).  Before the write, GE_GUARD_GVRAM(the X68000 address)
 * (waits for the GE if it still reads the word).  It bumps the row's
 * generation (the GE's GVRAM copies) and, once 65536 colours were shown,
 * converts the word into the GE's 65536 colour dots (GE_G16Write, about
 * 40 cycles).
 */
extern int GE_G16Live;
void GE_G16Write(DWORD a);
/* GVRAM bytes a .. a + n - 1 changed (word aligned), e.g. by a fast clear: as GE_GVRAM_ROW for each word */
void GE_GvramSpan(DWORD a, DWORD n);
#define GE_GVRAM_ROW(a)		do { if (GE_G16Live) GE_G16Write(a); \
				     else GE_GRowGen[((a) >> 10) & 511]++; } while (0)
/* TVRAM_Write: the TextDrawWork byte at a * 8 changed */
#define GE_TVRAM_TOUCH(a)	(GE_TRowGen[((a) >> 7) & 0x3ff]++)
/* palette.c: Pal16 is about to change (bumped after the bands drawn with it) */
extern DWORD GE_Pal16Gen;
/* windraw.c: ScrBufL row y was written by the CPU */
void GE_ScrRowWritten(DWORD y);

#define GE_GUARD_GVRAM(a)	do { if (GE_Guard) GE_GvramGuard(a); } while (0)
#define GE_GUARD_TVRAM(a)	do { if (GE_Guard) GE_TvramGuard(a); } while (0)
#define GE_GUARD_SPRITE()	do { if (GE_Guard) GE_SpriteGuard(); } while (0)
#define GE_GUARD_FULL(why)	do { if (GE_Guard) GE_FullGuard(why); } while (0)

/* windraw.c, WinDraw_DrawLine: 1 if line VLINE is left to the GE */
int GE_Line(void);
/* bands are waiting to be drawn */
int GE_Pending(void);
/*
 * The drawing of the waiting bands as a call list (GU_CALL, ends with RET)
 * in the GE's list memory; fbp: draw buffer to switch back to.  Nothing
 * runs before the caller calls the list (sceGuCallList), after it has
 * written the D-cache back (vertices, GVRAM, TextDrawWork).  GE_Done()
 * once the GE has finished all lists; the list memory is reused then.
 * GE_Room(): the waiting bands fit in what is left of the list memory (they
 * always do when the GE is idle).
 */
void *GE_Build(void *fbp, int passes);
int GE_Room(void);
/* GE_Room, and no BG writes wait for the GE's lists: a list may be queued now */
int GE_CanKick(void);
/* list memory (64-byte aligned), for a direct list that calls a GE_Build list */
void *GE_ListMem(int size);
void GE_Done(void);

/* GE_Render passes, in this order ("ge time" times them one by one) */
enum {
	GE_P_COPY = 1,		/* GVRAM / text rows to VRAM */
	GE_P_FILL = 2,		/* layer: fill (TextPal[0], depth) */
	GE_P_BGB = 4,		/* layer: BG/sprites below the text */
	GE_P_TEXT = 8,		/* layer: text */
	GE_P_BGA = 16,		/* layer: BG/sprites above the text */
	GE_P_GRP = 32,		/* screen: graphic pages */
	GE_P_COMP = 64,		/* screen: the layer over them */
	GE_P_END = 128,		/* the bands are done */
	GE_P_ALL = 255,
	GE_NPASS = 7
};

/* BG[adr] (0-0x7fff) is about to become data: guard, and the GE's pattern copies */
void GE_BGData(DWORD adr, BYTE data);
void GE_BGReset(void);

/* statistics for the "ge" debug command */
enum {
	GE_ST_GVRAM,		/* waits: GVRAM write on a shown dot */
	GE_ST_GVRAM_MODE,	/*   GVRAM write in another memory layout */
	GE_ST_TVRAM,		/*   text write on a shown dot */
	GE_ST_FASTCLR,		/*   GVRAM fast clear */
	GE_ST_RCUPD,		/*   text raster copy */
	GE_ST_BG,		/*   BG map/pattern write on something used */
	GE_ST_BGQ_FULL,		/*   too many BG writes while the GE draws */
	GE_ST_SPR_FULL,		/*   too many sprite register copies */
	GE_ST_INFLIGHT,		/*   (unused: the first line no longer waits for the last frame) */
	GE_ST_BANDS,		/*   too many bands */
	GE_ST_PALS,		/*   too many palettes */
	GE_ST_HALF,		/* lists: the bands of lines 0-255 drawn when line 256 comes */
	GE_ST_SPR_COPY,		/* sprite register copies */
	GE_ST_BG_SCAN,		/* BG use scans */
	GE_ST_RENDERS,		/* display lists with bands */
	GE_ST_DRAWS,		/* sceGuDrawArray calls */
	GE_ST_VERTS,		/* vertices */
	GE_ST_PIXELS,		/* pixels of the textured sprites */
	GE_ST_WAIT_US,		/* CPU time waiting for the GE (guards) */
	GE_ST_RENDER_US,	/* CPU time building the bands' display lists */
	GE_ST_GE_US,		/* "ge time": GE time of the frame's display list */
	GE_ST_PASS_US,		/* "ge time": + log2(GE_P_*): GE time of each pass */
	GE_ST_COPY_BYTES = GE_ST_PASS_US + GE_NPASS,	/* bytes copied to VRAM */
	GE_ST_FRAME_WAIT_US,	/* CPU time waiting for the GE when a frame is shown */
	GE_ST_LINE_WAITS,	/* CPU lines that waited for the GE */
	GE_ST_LINE_WAIT_US,	/* ... and how long */
	GE_ST_DONE_US,		/* CPU time of GE_Done (queued BG writes) */
	GE_ST_BUILD_US,		/* + 0-4: time of the build: CLUTs/setup, copy, layer, screen, D-cache; + 5: this thread's CPU time of it */
	GE_ST_LAYER_US = GE_ST_BUILD_US + 6,	/* + 0-3: of the layer: fill, "gd" rectangles, sprites/BG planes, text */
	GE_ST_G16_DOTS = GE_ST_LAYER_US + 4,	/* 65536 colour dots converted by the CPU */
	GE_ST_G16_WHY,		/* + 0-4: rows converted: never, palette, GenAll, row written, columns */
	GE_ST_G16_FAST = GE_ST_G16_WHY + 5,	/* ... of the dots converted: by the 512 byte tables */
	GE_ST_G16_GE,		/* 65536 colour dots the GE converted */
	GE_ST_G16_US,		/* CPU time converting 65536 colour dots */
	GE_ST_FRAMES,		/* frames shown */
	GE_ST_CPU_REASON,	/* + GE_R_*: lines left to the CPU, by reason */
	GE_ST_N = GE_ST_CPU_REASON + 10
};

enum {
	GE_R_DEBUG = 1,		/* Debug_Text/Grp/Sp off */
	GE_R_GMODE,		/* not 16 colour 512 dot graphics */
	GE_R_TRANS,		/* translucency / special priority */
	GE_R_R29,		/* CRTC R20 & 0x1c == 0x1c */
	GE_R_WIDTH,		/* wider than 512 dots */
	GE_R_PRIO,		/* graphics between text and BG */
	GE_R_TWRAP,		/* text line wraps at 1024 */
	GE_R_BGRES,		/* BG and CRTC resolutions differ */
	GE_R_VLINE		/* line 256 or below */
};

extern unsigned GE_Stat[GE_ST_N];
extern unsigned GE_StatLines, GE_StatBands, GE_StatFlushes, GE_StatCpuLines;
void GE_LogStats(void);

#ifdef __cplusplus
}
#endif

#else /* !PSP */

#define GE_GUARD_GVRAM(a)	do { } while (0)
#define GE_GUARD_TVRAM(a)	do { } while (0)
#define GE_GUARD_SPRITE()	do { } while (0)
#define GE_GUARD_FULL(why)	do { } while (0)
#define GE_GVRAM_ROW(a)		do { } while (0)
#define GE_TVRAM_TOUCH(a)	do { } while (0)

#endif /* PSP */

#endif
