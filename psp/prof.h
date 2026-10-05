#ifndef PX68K_PSP_PROF_H
#define PX68K_PSP_PROF_H

/*
 * Where the time goes (debug command "prof on", builds with -DPROF_TIMERS):
 * microseconds per category, summed by the frame-rate log and reset every
 * second. Off, a section costs one load and branch; on, two
 * sceKernelGetSystemTimeLow() calls.
 *
 * On the device these timers cost more than what they time (thousands of
 * system calls per frame: benchf of Gradius' demo 30.8 ms profiled, 12.1 ms
 * not); the sampling profiler below does not.
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
	PROF_ADPCMPRE,	/* ADPCM_PreUpdate (incl. its DMA) */
	PROF_OPMTIMER,
	PROF_MCRY,
	PROF_LINE,	/* per raster line: interrupts, sound/OPM/MIDI timers, keyboard, SCC (minus synthesis) */
	PROF_N
};

enum {
	PROF_LINES,	/* lines composited */
	PROF_FRAMES,	/* frames presented */
	PROF_SOUND_SAMPLES,
	PROF_COUNT_N
};

/*
 * Sampling profiler (debug commands "samp <sec> [period us]" and
 * "benchf ... prof 2"): the code says what it is doing with one byte store
 * (PROF_SET, or PROF_ENTER/PROF_LEAVE around what can run inside another
 * section), and an alarm interrupt (psp/prof.c) counts the section it finds
 * every ~period us.  A section's share of the samples times the measured
 * time per frame is its time per frame.  The stores cost a few thousand
 * instructions per frame, the alarm one interrupt per sample: ~22 us on a
 * PSP Go at 333 MHz (benchf 14.5 -> 15.9 ms/frame at a 250 us period), so
 * the default period is 1000 us (~2 %, inflating all sections alike).  The sound
 * callback runs on a thread of its own, preempting the emulator: it enters
 * its own section and restores the emulator's when it returns.
 */
enum {
	PS_IDLE,	/* main loop outside WinX68k_Exec: waiting for the sound (the headroom), menu, input */
	PS_DEBUG,	/* frame rate / debug bookkeeping after each frame */
	PS_CPU,		/* C68k_Exec, minus the slow memory paths below */
	PS_IO_GVRAM,	/* slow memory paths (mem_wrap.c *_slow) by area: GVRAM, incl. GE hooks */
	PS_IO_TVRAM,	/*   text VRAM */
	PS_IO_VID,	/*   CRTC, video controller, palette */
	PS_IO_BG,	/*   BG / sprites */
	PS_IO_SND,	/*   OPM, ADPCM */
	PS_IO_OTHER,	/*   other I/O, odd / bus error accesses */
	PS_SLICE,	/* per 68000 slice: MFP/RTC timers, DMA */
	PS_LINE,	/* per raster line: interrupts, sound/OPM/MIDI timers, keyboard, SCC, sample counting */
	PS_DL_MIX,	/* WinDraw_DrawLine on the CPU: compositing, minus the decoders below */
	PS_DL_GRP,	/*   graphic plane line decode */
	PS_DL_TEXT,	/*   text line decode */
	PS_DL_BG,	/*   BG / sprite line decode */
	PS_GE_LINE,	/* GE_Line: a line's state recorded for the GE */
	PS_GE_GUARD,	/* GE guards: masks, sprite register copies, BG write queue */
	PS_GE_BUILD,	/* the GE's display lists (GE_Build) and the D-cache writeback */
	PS_GE_WAIT,	/* waiting for the GE (sceGuSync) */
	PS_DRAW,	/* WinDraw_Draw, minus the GE build / wait */
	PS_FRAME,	/* end of frame: fast clear, joystick, FDD, frame skip */
	PS_SYN_ADPCM,	/* ADPCM synthesis on the emulator thread */
	PS_SYN_OPM,	/* OPM synthesis on the emulator thread */
	PS_CB,		/* sound callback (its thread): copy, smoothing, mixing */
	PS_CB_SYN,	/* sound callback: synthesis of a shortfall */
	PS_N
};

/* events, counted always (an increment per slice / line / slow access) */
enum {
	PEV_FRAMES,	/* WinX68k_Exec calls */
	PEV_DRAWN,	/* frames drawn (WinDraw_Draw) */
	PEV_SLICES,	/* C68k_Exec calls */
	PEV_HLINES,	/* raster lines emulated */
	PEV_CPULINES,	/* lines composited by the CPU */
	PEV_GELINES,	/* lines left to the GE */
	PEV_IO,		/* + PS_IO_* - PS_IO_GVRAM: slow memory accesses by area */
	PEV_SYN_SAMPLES = PEV_IO + 6,	/* samples synthesized on the emulator thread */
	PEV_CB,		/* sound callbacks */
	PEV_CB_SAMPLES,	/* samples synthesized by the callback */
	PEV_IDLE_SLICES,	/* slices run by C68k_Exec_Idle, without the core */
	PEV_N
};

/* __psp__: psp-gcc only (host test builds of x68k/ define PSP too) */
#ifdef __psp__
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

#define PROF_COUNT(id, n) do { prof_count[id] += (n); } while (0)
#else
#define PROF_COUNT(id, n) do { } while (0)
#endif
/*
 * The timers only with -DPROF_TIMERS (make -f Makefile.psp XCFLAGS=-DPROF_TIMERS):
 * even off, their tests cost ~25k instructions per frame (2 per slice, 5 per line).
 */
#if defined(__psp__) && defined(PROF_TIMERS)
#define PROF_BEGIN(name) unsigned prof_t_##name = prof_on ? sceKernelGetSystemTimeLow() : 0
#define PROF_END(name, id) \
	do { if (prof_on) prof_us[id] += sceKernelGetSystemTimeLow() - prof_t_##name; } while (0)
#else
#define PROF_BEGIN(name)
#define PROF_END(name, id) do { } while (0)
#endif

#ifdef __psp__
#ifdef __cplusplus
extern "C" {
#endif
/*
 * PS_*: what the emulator thread does.  Not small data (.data): the alarm
 * handler reads it, and an interrupt handler need not run with this
 * module's $gp (builds with -G8).
 */
extern volatile unsigned char prof_cur __attribute__((section(".data")));
extern unsigned prof_ev[PEV_N];

/* sample every ~period_us from now on; 0 or the alarm's error */
int prof_samp_start(unsigned period_us);
void prof_samp_stop(void);
/*
 * Log the samples (and events) since prof_samp_start as ms per frame: us
 * is the wall time of the window, frames the frames it ran.
 */
void prof_samp_report(const char *tag, unsigned us, unsigned frames);
#ifdef __cplusplus
}
#endif

#define PROF_SET(s)	(prof_cur = (s))
#define PROF_ENTER(s)	unsigned char prof_prev_ = prof_cur; prof_cur = (s)
#define PROF_LEAVE()	(prof_cur = prof_prev_)
#define PROF_EV(id, n)	(prof_ev[id] += (n))
/* the slow memory path section of X68000 address a */
#define PROF_IO_SEC(a)	((((a) & 0xffffff) - 0xc00000 < 0x200000) ? PS_IO_GVRAM : \
			 (((a) & 0xffffff) - 0xe00000 < 0x80000) ? PS_IO_TVRAM : \
			 (((a) & 0xffffff) - 0xe80000 < 0x4000) ? PS_IO_VID : \
			 (((a) & 0xffffff) - 0xeb0000 < 0x10000) ? PS_IO_BG : \
			 (((a) & 0xffffff) - 0xe90000 < 0x4000) ? PS_IO_SND : PS_IO_OTHER)
#define PROF_IO_ENTER(a) \
	PROF_ENTER(PROF_IO_SEC(a)); PROF_EV(PEV_IO + prof_cur - PS_IO_GVRAM, 1)
#else
#define PROF_SET(s)	do { } while (0)
#define PROF_ENTER(s)	do { } while (0)
#define PROF_LEAVE()	do { } while (0)
#define PROF_EV(id, n)	do { } while (0)
#define PROF_IO_ENTER(a)	do { } while (0)
#endif

#endif
