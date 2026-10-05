#include <pspkernel.h>
#include <psputils.h>
#include <kubridge.h>
#include <string.h>

#include "me.h"
#include "log.h"

#define ME_STACK_SIZE	(32 * 1024)

/* Read by me_entry (psp/me_boot.S) uncached; a cache line of its own. */
struct me_boot {
	unsigned sp, gp;
	void (*loop)(void);
	unsigned halt;		/* main -> ME: return from loop */
	unsigned state;		/* ME -> main: ME_RUNNING, ME_HALTED */
	unsigned pad[11];
};
#define ME_RUNNING	0x4d45ac01
#define ME_HALTED	0x4d45ac02

struct me_boot me_boot __attribute__((aligned(64)));
static unsigned char me_stack[ME_STACK_SIZE] __attribute__((aligned(64)));
static int me_started, me_paused;
static void (*me_loop_fn)(void);

extern char me_reset[], me_reset_end[];

static volatile struct me_boot *boot(void)
{
	return (volatile struct me_boot *)ME_UNCACHED(&me_boot);
}

/* Write back and invalidate this CPU's data cache (16 KB, 2 ways, 64-byte lines). */
static void dcache_wbinv_all(void)
{
	int i;

	__asm__ volatile("sync");
	for (i = 0; i < 8192; i += 64) {
		__asm__ volatile("cache 0x14, 0(%0)" :: "r"(i));
		__asm__ volatile("cache 0x14, 0(%0)" :: "r"(i));
	}
	__asm__ volatile("sync");
}

/* On the ME (me_entry). */
void me_main(void)
{
	volatile struct me_boot *b = boot();

	b->state = ME_RUNNING;
	b->loop();
	/* Nothing of ours may be written back later over the next program. */
	dcache_wbinv_all();
	b->state = ME_HALTED;
}

/* Kernel mode (kuKernelCall): put me_reset at the ME's reset vector and reset it. */
static int me_reset_k(void)
{
	memcpy((void *)0xbfc00000, me_reset, me_reset_end - me_reset);
	dcache_wbinv_all();
	__asm__ volatile("sync");
	*(volatile unsigned *)0xbc10004c = 0x04;	/* ME reset on */
	__asm__ volatile("sync");
	*(volatile unsigned *)0xbc10004c = 0;
	__asm__ volatile("sync");
	return 0;
}

int me_start(void (*loop)(void))
{
	volatile struct me_boot *b = boot();
	KernelCallArg args;
	unsigned gp;
	int i, ret;

	if (me_started)
		return 0;
	__asm__ volatile("move %0, $gp" : "=r"(gp));
	sceKernelDcacheWritebackInvalidateRange(&me_boot, sizeof(me_boot));
	b->sp = (unsigned)me_stack + ME_STACK_SIZE - 64;
	b->gp = gp;
	b->loop = loop;
	b->halt = 0;
	b->state = 0;
	/* What the ME reads cached (code, tables, its objects) must be in RAM. */
	sceKernelDcacheWritebackInvalidateAll();
	memset(&args, 0, sizeof(args));
	ret = kuKernelCall((void *)me_reset_k, &args);
	if (ret < 0) {
		log_printf("me: kuKernelCall failed %08X\n", ret);
		return -1;
	}
	for (i = 0; i < 200 && b->state != ME_RUNNING; i++)
		sceKernelDelayThread(1000);
	if (b->state != ME_RUNNING) {
		log_printf("me: no answer from the Media Engine\n");
		return -1;
	}
	me_started = 1;
	me_loop_fn = loop;
	return 0;
}

int me_halting(void)
{
	return boot()->halt;
}

int me_running(void)
{
	return me_started && boot()->state == ME_RUNNING;
}

int me_paused_now(void)
{
	return me_paused;
}

void me_pause(void)
{
	if (!me_started)
		return;
	me_paused = 1;	/* first: the sound callback stops waiting for it */
	me_halt();
}

int me_resume(void)
{
	if (!me_paused)
		return 0;
	me_paused = 0;
	return me_start(me_loop_fn);
}

void me_halt(void)
{
	volatile struct me_boot *b = boot();
	int i;

	if (!me_started)
		return;
	b->halt = 1;
	for (i = 0; i < 100 && b->state != ME_HALTED; i++)
		sceKernelDelayThread(1000);
	me_started = 0;
}
