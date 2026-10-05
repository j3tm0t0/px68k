#include <pspkernel.h>
#include <psputils.h>
#include <kubridge.h>
#include <pspsysevent.h>
#include <systemctrl.h>
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
 * The firmware's own reset handler, which ours replaces.  It goes back, and
 * the firmware's ME core is restarted with it, whenever we halt (me_halt,
 * me_pause): before px68k's memory goes away.  0xbfc00600 on holds the
 * firmware core's parameters (custom-core, safe-task): not ours to touch.
 */
#define ME_VECTOR_SAVE	0x200
static unsigned me_vector[ME_VECTOR_SAVE / 4];
static int me_vector_saved;
static int me_installed;	/* our handler is at the reset vector */
static int me_started, me_paused, me_starts;

/*
 * The firmware talks to its ME core from the "SceMeRpc" system event handler
 * of me_wrapper (uofw src/kd/me_wrapper): on a clock change (events 0x1000002
 * and 0x1000020) and around a suspend it sends the core a request and waits,
 * without a time limit, for its answer.  With our code on the ME there is no
 * answer: that hung the PSP at "bench" (the WLAN going down raises the clock),
 * also with the firmware's reset handler put back and its core restarted.
 * So while px68k owns the ME, that handler's events are dropped (as
 * custom-core's kinit replaces it); px68k starts the ME only once that hook
 * is in place, and takes it out at exit.
 */
static PspSysEventHandler *me_rpc;
static PspSysEventHandlerFunc me_rpc_orig;
static int me_hooked;
static void (*me_loop_fn)(void);

extern char me_reset[], me_reset_end[], me_reset_open[], me_reset_open_end[];
unsigned me_clocks = 0x0f;	/* the ME's 0xbc100050 (debug "me clk") */

int me_halt(void);

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
	*(volatile unsigned *)0xbfc00008 = me_clocks;	/* ME_CLOCKS in me_boot.S */
	me_installed = 1;
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
	int i, bad = 0;

	for (i = 0; i < ME_VECTOR_SAVE / 4; i++)
		((volatile unsigned *)0xbfc00000)[i] = me_vector[i];
	__asm__ volatile("sync");
	for (i = 0; i < ME_VECTOR_SAVE / 4; i++)
		bad += ((volatile unsigned *)0xbfc00000)[i] != me_vector[i];
	if (bad)
		return bad;	/* no reset into a damaged handler */
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

/*
 * Called by the kernel for SceMeRpc's events while hooked: none reaches the
 * firmware's core.  0x4005 (16389) is the last step before a suspend, where
 * me_wrapper has its core save itself and then holds the ME in reset: here
 * our loop is stopped (its cache written back) and the ME held in reset, in
 * that same synchronous step; the power callback restarts it on resume.  No
 * system calls: this runs inside the suspend sequence.
 */
static int me_sysevent(int ev_id, char *ev_name, void *param, int *result)
{
	volatile struct me_boot *b = boot();
	int i;

	if (ev_id == 0x4005 && me_started) {
		b->halt = 1;
		for (i = 0; i < 20000 && b->state != ME_HALTED; i++)
			me_spin(1000);	/* about 60 ms at most */
		*(volatile unsigned *)0xbc10004c |= 0x04;	/* ME reset on */
		__asm__ volatile("sync");
		me_started = 0;
		me_paused = 1;
	}
	return 0;
}

/* Kernel mode: arg1 = sceKernelReferSysEventHandler. */
static int me_hook_k(unsigned refer)
{
	PspSysEventHandler *h = ((PspSysEventHandler *(*)(void))refer)();

	for (; h; h = h->next) {
		if (h->name && strcmp(h->name, "SceMeRpc") == 0) {
			me_rpc = h;
			me_rpc_orig = h->handler;
			h->handler = me_sysevent;
			return 1;
		}
	}
	return 0;
}

static int me_unhook_k(void)
{
	me_rpc->handler = me_rpc_orig;
	return 0;
}

/* f(arg) in kernel mode; f's return value, or < 0 if the call failed. */
static int me_kcall(void *f, unsigned arg)
{
	KernelCallArg args;
	int ret;

	memset(&args, 0, sizeof(args));
	args.arg1 = arg;
	ret = kuKernelCall(f, &args);
	return ret < 0 ? ret : (int)args.ret1;
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

static int me_hook(void)
{
	unsigned refer;

	if (me_hooked)
		return 0;
	/* sysmem's sceSysEventForKernel; the NID is not randomized (uofw sysmem exports) */
	refer = sctrlHENFindFunction("sceSystemMemoryManager", "sceSysEventForKernel", 0x68D55505);
	if ((refer & 0xf0000000) != 0x80000000) {
		log_printf("me: sceKernelReferSysEventHandler not found (%08X)\n", refer);
		return -1;
	}
	if (me_kcall((void *)me_hook_k, refer) != 1) {
		log_printf("me: no SceMeRpc event handler to take over\n");
		return -1;
	}
	me_hooked = 1;
	return 0;
}

int me_start(void (*loop)(void))
{
	volatile struct me_boot *b = boot();
	unsigned gp;
	int open = 0;

	if (me_started)
		return 0;
	if (me_hook() < 0)
		return -1;	/* the ME stays the firmware's */
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
			me_halt();
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
			me_halt();
			return -1;
		}
		b->go = 1;
		if (!me_wait(ME_RUNNING, 100)) {
			me_halt();
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

int me_clock_safe(void)
{
	return !me_installed || me_hooked;
}

void me_release(void)
{
	me_halt();
	if (me_hooked) {
		me_kcall((void *)me_unhook_k, 0);
		me_hooked = 0;
	}
}

int me_pause(void)
{
	if (!me_installed)
		return 0;
	me_paused = 1;	/* first: the sound callback stops waiting for it */
	return me_halt();
}

int me_resume(void)
{
	if (!me_paused)
		return 0;
	me_paused = 0;
	return me_start(me_loop_fn);
}

int me_halt(void)
{
	volatile struct me_boot *b = boot();
	int i, ret;

	if (me_started) {
		b->halt = 1;
		for (i = 0; i < 100 && b->state != ME_HALTED; i++)
			sceKernelDelayThread(1000);
		if (b->state != ME_HALTED)
			log_printf("me: the loop did not stop (state %08X); reset anyway\n", b->state);
		me_started = 0;
	}
	if (!me_installed)
		return 0;
	/* the reset stops whatever the ME runs; then the firmware's handler has it */
	ret = me_kcall((void *)me_giveback_k, 0);
	if (ret != 0) {
		log_printf("me: the firmware's reset handler could not be put back (%d)\n", ret);
		return -1;
	}
	me_installed = 0;
	sceKernelDelayThread(10 * 1000);	/* let the firmware's core come up */
	return 0;
}
