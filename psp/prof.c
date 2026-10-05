#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "prof.h"

/*
 * The sampling profiler of prof.h: an alarm (an interrupt handler: no
 * system calls, nothing but a few stores) counts prof_cur and re-arms
 * itself with a little jitter, so that it does not lock onto the period of
 * the raster lines or slices.
 */
volatile unsigned char prof_cur __attribute__((section(".data")));
unsigned prof_ev[PEV_N];

static volatile unsigned prof_hist[PS_N];
/* what the handler uses: not small data either (prof.h) */
static volatile int prof_running __attribute__((section(".data")));
static unsigned prof_period __attribute__((section(".data")));
static unsigned prof_rnd __attribute__((section(".data"))) = 1;
static SceUID prof_alarm = -1;
static unsigned prof_ev0[PEV_N];
static unsigned prof_run0;	/* this thread's run time at the start, us */
static unsigned prof_t0;
/* every thread's run time at the start: where the emulator thread's wall time goes */
#define PROF_NTH	48
static SceUID prof_thid[PROF_NTH];
static unsigned prof_thrun[PROF_NTH];
static int prof_nth;

static SceUInt prof_tick(void *common)
{
	(void)common;
	prof_hist[prof_cur < PS_N ? prof_cur : PS_IDLE]++;
	if (!prof_running)
		return 0;
	prof_rnd = prof_rnd * 1664525u + 1013904223u;
	return prof_period - 8 + (prof_rnd >> 28);	/* +-8 us */
}

static unsigned prof_run_us(void)
{
	SceKernelThreadInfo ti;

	memset(&ti, 0, sizeof(ti));
	ti.size = sizeof(ti);
	if (sceKernelReferThreadStatus(0, &ti) < 0)
		return 0;
	return ti.runClocks.low;
}

static int prof_thread(SceUID id, SceKernelThreadInfo *ti)
{
	memset(ti, 0, sizeof(*ti));
	ti->size = sizeof(*ti);
	return sceKernelReferThreadStatus(id, ti) >= 0;
}

static void prof_threads_start(void)
{
	SceKernelThreadInfo ti;
	int i, n = 0;

	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, prof_thid, PROF_NTH, &n) < 0)
		n = 0;
	prof_nth = n < PROF_NTH ? n : PROF_NTH;
	for (i = 0; i < prof_nth; i++)
		prof_thrun[i] = prof_thread(prof_thid[i], &ti) ? ti.runClocks.low : 0;
}

/* run time of each thread since prof_threads_start, ms per frame (the user threads this module can see) */
static void prof_threads_report(const char *tag, unsigned wall, unsigned frames)
{
	SceKernelThreadInfo ti;
	char line[1024];
	unsigned sum = 0;
	int i, n;

	n = snprintf(line, sizeof(line), "samp %s: threads (ms/frame, %% of %u ms):", tag, wall / 1000);
	for (i = 0; i < prof_nth && n < (int)sizeof(line); i++) {
		unsigned r;

		if (!prof_thread(prof_thid[i], &ti))
			continue;
		r = ti.runClocks.low - prof_thrun[i];
		sum += r;
		if (r < wall / 1000)
			continue;	/* under 0.1 % */
		n += snprintf(line + n, sizeof(line) - n, " %.31s %u.%02u (%u%%)", ti.name,
			      r / frames / 1000, r / frames / 10 % 100,
			      wall ? (unsigned)((unsigned long long)r * 100 / wall) : 0);
	}
	if (n < (int)sizeof(line))
		snprintf(line + n, sizeof(line) - n, "; all listed %u%%, the rest (interrupts, kernel, idle) %u.%02u ms/frame",
			 wall ? (unsigned)((unsigned long long)sum * 100 / wall) : 0,
			 wall > sum ? (wall - sum) / frames / 1000 : 0, wall > sum ? (wall - sum) / frames / 10 % 100 : 0);
	log_printf("%s\n", line);
}

int prof_samp_start(unsigned period_us)
{
	int i;

	prof_samp_stop();
	if (period_us < 20)
		period_us = 20;
	prof_period = period_us;
	for (i = 0; i < PS_N; i++)
		prof_hist[i] = 0;
	memcpy(prof_ev0, prof_ev, sizeof(prof_ev0));
	prof_threads_start();
	prof_run0 = prof_run_us();
	prof_t0 = sceKernelGetSystemTimeLow();
	prof_running = 1;
	prof_alarm = sceKernelSetAlarm(period_us, prof_tick, NULL);
	if (prof_alarm < 0) {
		prof_running = 0;
		return prof_alarm;
	}
	return 0;
}

void prof_samp_stop(void)
{
	prof_running = 0;	/* the handler does not re-arm itself */
	if (prof_alarm >= 0) {
		sceKernelCancelAlarm(prof_alarm);	/* fails if it has just ended: fine */
		prof_alarm = -1;
	}
}

static const char *const prof_names[PS_N] = {
	"idle", "debug", "cpu", "io.gvram", "io.tvram", "io.vid", "io.bg", "io.snd", "io.other",
	"slice", "line", "dl.mix", "dl.grp", "dl.text", "dl.bg", "ge.line", "ge.guard",
	"ge.build", "ge.wait", "draw", "frame", "syn.adpcm", "syn.opm", "cb", "cb.syn"
};

void prof_samp_report(const char *tag, unsigned us, unsigned frames)
{
	unsigned hist[PS_N], ev[PEV_N], total = 0, run, wall;
	char line[1024];
	int i, n;

	for (i = 0; i < PS_N; i++)
		total += hist[i] = prof_hist[i];
	for (i = 0; i < PEV_N; i++)
		ev[i] = prof_ev[i] - prof_ev0[i];
	run = prof_run_us() - prof_run0;
	wall = sceKernelGetSystemTimeLow() - prof_t0;
	if (!frames)
		frames = 1;
	if (!total)
		total = 1;
	/* section time per frame in units of 10 us: share of the samples x us per frame */
	/* the emulator thread's run time (PPSSPP: 0) vs the wall time: other threads, idle */
	n = snprintf(line, sizeof(line), "samp %s: %u frames (%u drawn), %u us/frame, %u samples / %u us, "
		     "emulator thread ran %u%% of %u ms; ms/frame:", tag, frames, ev[PEV_DRAWN], us / frames,
		     total, prof_period, wall ? (unsigned)((unsigned long long)run * 100 / wall) : 0, wall / 1000);
	for (i = 0; i < PS_N; i++) {
		unsigned t;

		if (!hist[i])
			continue;
		t = (unsigned)((unsigned long long)hist[i] * us / total / frames / 10);
		n += snprintf(line + n, sizeof(line) - n, " %s %u.%02u", prof_names[i], t / 100, t % 100);
		if (n >= (int)sizeof(line))
			break;
	}
	log_printf("%s\n", line);
	/* events per frame, x100 */
#define PF(id) (unsigned)((unsigned long long)ev[id] * 100 / frames / 100), \
	       (unsigned)((unsigned long long)ev[id] * 100 / frames % 100)
	log_printf("samp %s: per frame: slices %u.%02u lines %u.%02u cpulines %u.%02u gelines %u.%02u "
		   "slow io gvram %u.%02u tvram %u.%02u vid %u.%02u bg %u.%02u snd %u.%02u other %u.%02u "
		   "synth samples %u.%02u callbacks %u.%02u callback samples %u.%02u idle slices %u.%02u\n", tag,
		   PF(PEV_SLICES), PF(PEV_HLINES), PF(PEV_CPULINES), PF(PEV_GELINES),
		   PF(PEV_IO), PF(PEV_IO + 1), PF(PEV_IO + 2), PF(PEV_IO + 3), PF(PEV_IO + 4), PF(PEV_IO + 5),
		   PF(PEV_SYN_SAMPLES), PF(PEV_CB), PF(PEV_CB_SAMPLES), PF(PEV_IDLE_SLICES));
#undef PF
	prof_threads_report(tag, wall, frames);
}
