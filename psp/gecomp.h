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

void GE_GvramGuard(DWORD adr);	/* GVRAM_Write, its address */
void GE_TvramGuard(DWORD adr);	/* TVRAM_Write, its address */
void GE_SpriteGuard(void);	/* a sprite register changes */
void GE_FullGuard(int why);	/* anything else: wait (why: GE_ST_*) */

#define GE_GUARD_GVRAM(a)	do { if (GE_Guard) GE_GvramGuard(a); } while (0)
#define GE_GUARD_TVRAM(a)	do { if (GE_Guard) GE_TvramGuard(a); } while (0)
#define GE_GUARD_SPRITE()	do { if (GE_Guard) GE_SpriteGuard(); } while (0)
#define GE_GUARD_FULL(why)	do { if (GE_Guard) GE_FullGuard(why); } while (0)

/* windraw.c, WinDraw_DrawLine: 1 if line VLINE is left to the GE */
int GE_Line(void);
/* bands are waiting to be drawn */
int GE_Pending(void);
/*
 * Put the drawing of the waiting bands into the open display list (between
 * sceGuStart and sceGuFinish); fbp: draw buffer to switch back to.  The
 * caller writes the D-cache back before sceGuFinish and calls GE_Done()
 * once the GE has finished the list.
 */
void GE_Render(void *fbp);
void GE_Done(void);

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
	GE_ST_INFLIGHT,		/*   first line while the GE still draws the last frame */
	GE_ST_BANDS,		/*   too many bands */
	GE_ST_PALS,		/*   too many palettes */
	GE_ST_SPR_COPY,		/* sprite register copies */
	GE_ST_BG_SCAN,		/* BG use scans */
	GE_ST_RENDERS,		/* display lists with bands */
	GE_ST_DRAWS,		/* sceGuDrawArray calls */
	GE_ST_VERTS,		/* vertices */
	GE_ST_PIXELS,		/* pixels of the textured sprites */
	GE_ST_WAIT_US,		/* CPU time waiting for the GE (guards) */
	GE_ST_RENDER_US,	/* CPU time building the bands' display lists */
	GE_ST_GE_US,		/* "ge time": GE time of the frame's list */
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

#endif /* PSP */

#endif
