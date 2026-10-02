#ifndef PX68K_PSP_GECOMP_H
#define PX68K_PSP_GECOMP_H

/*
 * GE compositing (PSP): the lines of the modes the GE can draw exactly like
 * the CPU path (x11/windraw.c DrawLine) are not composited by the CPU; their
 * register state is recorded per line, runs of lines with the same state
 * become bands, and the bands are drawn by the GE into the screen texture
 * (ScrBufL) from GVRAM, the text screen (TextDrawWork) and the BG patterns.
 *
 * The GE reads GVRAM, TextDrawWork, the BG patterns, the BG maps and the
 * sprite registers when the bands are drawn, so these must not change while
 * lines are waiting to be drawn or the GE is drawing them: whatever writes
 * them calls GE_GUARD() first.  The palettes and the registers are copied
 * per band, they need no guard.
 */

#ifdef PSP

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int GE_Enabled;		/* "ge on|off" */
extern volatile int GE_Guard;	/* lines are waiting for the GE, or the GE is drawing */
extern int GE_PalDirty;		/* set by TVRAM_SetAllDirty: palettes/registers may have changed */

/* windraw.c: draw the waiting lines now and wait until the GE is done */
void WinDraw_GESync(void);

#define GE_GUARD() do { if (GE_Guard) WinDraw_GESync(); } while (0)

/* windraw.c, WinDraw_DrawLine: 1 if line VLINE is left to the GE */
int GE_Line(void);
/* lines are waiting to be drawn */
int GE_Pending(void);
/*
 * Put the drawing of the waiting lines into the open display list (between
 * sceGuStart and sceGuFinish); fbp: draw buffer to switch back to.  The
 * caller writes the D-cache back before sceGuFinish.
 */
void GE_Render(void *fbp);

/* BG pattern shadows (bg.c) */
void GE_BGWrite(DWORD adr, BYTE data);
void GE_BGReset(void);

/* statistics for the "ge" debug command */
extern unsigned GE_StatLines, GE_StatBands, GE_StatFlushes, GE_StatCpuLines;

#ifdef __cplusplus
}
#endif

#else /* !PSP */

#define GE_GUARD() do { } while (0)

#endif /* PSP */

#endif
