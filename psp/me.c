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
	unsigned go;		/* main -> ME: run loop (the protection is set) */
	unsigned state;		/* ME -> main: ME_READY, ME_RUNNING, ME_HALTED */
	unsigned pad[10];
};
#define ME_READY	0x4d45ac00
#define ME_RUNNING	0x4d45ac01
#define ME_HALTED	0x4d45ac02

/* 0xbc000000-0xbc000044: main RAM protection, user access to hardware registers */
#define ME_PROT_REGS	18

struct me_boot me_boot __attribute__((aligned(64)));
static unsigned char me_stack[ME_STACK_SIZE] __attribute__((aligned(64)));
static unsigned me_probe;	/* read cached through its user address by the ME */
static unsigned me_prot[ME_PROT_REGS];
/*
 * The firmware's own reset handler, which ours replaces: the firmware resets
 * the ME itself (clock changes: the WLAN going up or down; suspend) and waits
 * for that handler's answer, so the PSP hung there with ours in place.  It
 * goes back, and the firmware's ME core is restarted with it, whenever we
 * halt (me_halt, me_pause).  0xbfc00600 on holds the handler's parameters
 * (custom-core, safe-task): not ours to touch.
 */
#define ME_VECTOR_SAVE	0x200
static unsigned me_vector[ME_VECTOR_SAVE / 4];
static int me_vector_saved;
static int me_started, me_paused, me_starts;
static void (*me_loop_fn)(void);

extern char me_reset[], me_reset_end[], me_reset_open[], me_reset_open_end[];

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

	b->state = ME_READY;
	while (!b->go)
		me_spin(64);
	/* main RAM through the user segment, as the loop's objects are */
	b->state = ME_RUNNING + *(volatile unsigned *)&me_probe;
	b->loop();
	/* Nothing of ours may be written back later over the next program. */
	dcache_wbinv_all();
	b->state = ME_HALTED;
}

/*
 * Kernel mode (kuKernelCall): put a handler at the ME's reset vector and
 * reset it. arg1: me_reset_open, after saving the protection it opens.
 */
static int me_reset_k(unsigned open)
{
	char *h = open ? me_reset_open : me_reset;
	char *e = open ? me_reset_open_end : me_reset_end;
	int i;

	if (open)
		for (i = 0; i < ME_PROT_REGS; i++)
			me_prot[i] = ((volatile unsigned *)0xbc000000)[i];
	if (!me_vector_saved) {
		for (i = 0; i < ME_VECTOR_SAVE / 4; i++)
			me_vector[i] = ((volatile unsigned *)0xbfc00000)[i];
		me_vector_saved = 1;
	}
	memcpy((void *)0xbfc00000, h, e - h);
	dcache_wbinv_all();
	__asm__ volatile("sync");
	*(volatile unsigned *)0xbc10004c = 0x04;	/* ME reset on */
	__asm__ volatile("sync");
	*(volatile unsigned *)0xbc10004c = 0;
	__asm__ volatile("sync");
	return 0;
}

/* Kernel mode: the firmware's reset handler back, and its ME core restarted with it. */
static int me_giveback_k(void)
{
	int i;

	for (i = 0; i < ME_VECTOR_SAVE / 4; i++)
		((volatile unsigned *)0xbfc00000)[i] = me_vector[i];
	dcache_wbinv_all();
	__asm__ volatile("sync");
	*(volatile unsigned *)0xbc10004c = 0x04;
	__asm__ volatile("sync");
	*(volatile unsigned *)0xbc10004c = 0;
	__asm__ volatile("sync");
	return 0;
}

/* Kernel mode: put back what me_reset_open opened. */
static int me_protect_k(void)
{
	int i;

	for (i = 0; i < ME_PROT_REGS; i++)
		((volatile unsigned *)0xbc000000)[i] = me_prot[i];
	__asm__ volatile("sync");
	return 0;
}

static int me_kcall(void *f, unsigned arg)
{
	KernelCallArg args;

	memset(&args, 0, sizeof(args));
	args.arg1 = arg;
	return kuKernelCall(f, &args);
}

static int me_wait(unsigned state, int ms)
{
	volatile struct me_boot *b = boot();

	while (b->state != state && ms-- > 0)
		sceKernelDelayThread(1000);
	return b->state == state;
}

/* Reset the ME into me_main (waiting for go); 1 = with the open handler. */
static int me_reset_into(int open)
{
	volatile struct me_boot *b = boot();
	int ret;

	b->state = 0;
	b->go = 0;
	/* What the ME reads cached (code, tables, its objects) must be in RAM. */
	sceKernelDcacheWritebackInvalidateAll();
	ret = me_kcall((void *)me_reset_k, open);
	if (ret < 0) {
		log_printf("me: kuKernelCall failed %08X\n", ret);
		return -1;
	}
	return me_wait(ME_READY, 200) ? 0 : -1;
}

int me_start(void (*loop)(void))
{
	volatile struct me_boot *b = boot();
	unsigned gp;
	int open = 0;

	if (me_started)
		return 0;
	__asm__ volatile("move %0, $gp" : "=r"(gp));
	sceKernelDcacheWritebackInvalidateRange(&me_boot, sizeof(me_boot));
	b->sp = (unsigned)me_stack + ME_STACK_SIZE - 64;
	b->gp = gp;
	b->loop = loop;
	b->halt = 0;
	if (me_reset_into(0) < 0) {
		if (b->state == 0 && me_reset_into(1) == 0)
			open = 1;
		else {
			log_printf("me: no answer from the Media Engine\n");
			me_kcall((void *)me_giveback_k, 0);
			return -1;
		}
	}
	if (open)
		me_kcall((void *)me_protect_k, 0);
	b->go = 1;
	if (!me_wait(ME_RUNNING, 100)) {
		/* it needs the protection open: leave it so (until the next reboot) */
		if (!open || me_reset_into(1) < 0) {
			log_printf("me: the Media Engine stopped (state %08X)\n", b->state);
			me_kcall((void *)me_giveback_k, 0);
			return -1;
		}
		b->go = 1;
		if (!me_wait(ME_RUNNING, 100)) {
			me_kcall((void *)me_giveback_k, 0);
			return -1;
		}
		log_printf("me: running with the memory protection open\n");
	} else if (open) {
		log_printf("me: started with the protection open, now closed again\n");
	}
	me_started = 1;
	me_starts++;
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

int me_start_count(void)
{
	return me_starts;
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
	if (me_vector_saved) {
		me_kcall((void *)me_giveback_k, 0);
		sceKernelDelayThread(10 * 1000);	/* let the firmware's core come up */
	}
}
