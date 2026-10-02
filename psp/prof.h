#ifndef PX68K_PSP_PROF_H
#define PX68K_PSP_PROF_H

/*
 * Where the time goes (debug command "prof on"): microseconds per category,
 * summed by the frame-rate log and reset every second. Off, a section costs
 * one load and branch; on, two sceKernelGetSystemTimeLow() calls.
 */
enum {
	PROF_CPU,	/* C68k_Exec */
	PROF_GRP,	/* graphic plane line decode (Grp_DrawLine*) */
	PROF_TEXT,	/* text plane line decode */
	PROF_BG,	/* BG / sprite line decode */
	PROF_MIX,	/* WinDraw_DrawLine compositing into ScrBuf, minus the above */
	PROF_DRAW,	/* WinDraw_Draw (GE upload, sync, swap) */
	PROF_SOUND,	/* ADPCM/OPM synthesis, either thread */
	PROF_SLICE,	/* per 68000 slice: MFP/RTC timers, DMA */
	PROF_LINE,	/* per raster line: interrupts, sound/OPM/MIDI timers, keyboard, SCC (minus synthesis) */
	PROF_N
};

enum {
	PROF_LINES,	/* lines composited */
	PROF_FRAMES,	/* frames presented */
	PROF_SOUND_SAMPLES,
	PROF_COUNT_N
};

#ifdef PSP
#include <psprtc.h>
#include <pspthreadman.h>

#ifdef __cplusplus
extern "C" {
#endif
extern int prof_on;
extern unsigned prof_us[PROF_N];
extern unsigned prof_count[PROF_COUNT_N];
#ifdef __cplusplus
}
#endif

#define PROF_BEGIN(name) unsigned prof_t_##name = prof_on ? sceKernelGetSystemTimeLow() : 0
#define PROF_END(name, id) \
	do { if (prof_on) prof_us[id] += sceKernelGetSystemTimeLow() - prof_t_##name; } while (0)
#define PROF_COUNT(id, n) do { prof_count[id] += (n); } while (0)
#else
#define PROF_BEGIN(name)
#define PROF_END(name, id) do { } while (0)
#define PROF_COUNT(id, n) do { } while (0)
#endif

#endif
