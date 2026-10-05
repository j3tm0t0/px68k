#ifndef PX68K_PSP_ME_H
#define PX68K_PSP_ME_H

/*
 * The Media Engine (the PSP's second Allegrex core) running a loop of ours.
 *
 * It is started at its reset vector (kernel mode, through the CFW's
 * kuKernelCall), so the firmware's own ME services are gone until the next
 * reboot: px68k uses none (no sceAudiocodec/MP3/AT3). It runs our code from
 * main RAM through its own caches, which are not coherent with the main
 * CPU's: data both sides touch goes through uncached addresses (ME_UNCACHED),
 * and the ME may only read cached what the main CPU wrote back before
 * me_start() (that writes back the whole data cache) and never changes again.
 * What the ME writes cached must sit in 64-byte lines of its own.
 *
 * After mcidclan's psp-media-engine-reload notes, me-minimal-handler and
 * psp-media-engine-custom-core (MIT, https://github.com/mcidclan).
 */

#ifdef __cplusplus
extern "C" {
#endif

#define ME_UNCACHED(p)	((void *)(0x40000000 | (unsigned)(p)))

/*
 * Reset the ME into loop() (on its own stack) and wait until it runs.
 * Returns 0, or < 0 when the ME could not be started (no kubridge, PPSSPP),
 * in which case nothing has changed.
 */
int me_start(void (*loop)(void));
/* Nonzero once loop() should return (me_halt); poll it in the loop. */
int me_halting(void);
/*
 * Make loop() return and halt the ME, before px68k's memory goes away
 * (exit, loadexec), or the ME runs on in whatever is loaded next.
 */
void me_halt(void);
int me_running(void);
/*
 * Halt the ME around clock changes (the WLAN going up or down changes the
 * PLL): it reads DDR all the time, and the PSP hung when the clock went to
 * 333 MHz with it running.  me_resume restarts loop() where it stopped
 * (everything it keeps is in RAM); returns < 0 if the ME did not come back.
 */
void me_pause(void);
int me_resume(void);
int me_paused_now(void);
/* Successful me_start calls so far (a clock change across a restart is fine). */
int me_start_count(void);

/* Busy-wait about n cycles without touching memory (ME polling loops). */
static inline void me_spin(int n)
{
	while (n-- > 0)
		__asm__ volatile("nop");
}

#ifdef __cplusplus
}
#endif

#endif
