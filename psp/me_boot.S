/*
 * Media Engine start-up (psp/me.c). me_reset is copied to the ME's reset
 * vector (0xbfc00000) by me_start; it is relocated with the module before
 * that, so its absolute addresses stay right. The register setup follows
 * meLibHandler of mcidclan's psp-media-engine-custom-core (MIT, Copyright (c)
 * 2026 mcidclan): with only the clocks and the memory access bit set (as in
 * his older me-minimal-handler), the ME never reached our code on a PSP Go.
 * Opening the protection registers also opens kernel memory to user mode
 * until the next reboot.
 */
	.set	noreorder
	.set	noat
	.text

	.globl	me_reset
	.globl	me_reset_end
me_reset:
	li	$k0, 0xbc100000
	li	$k1, 0x0f
	sw	$k1, 0x50($k0)		/* bus clocks: ME, AW bus A and B, eDRAM */
	lw	$k1, 0x40($k0)
	ori	$k1, $k1, 0x02
	sw	$k1, 0x40($k0)		/* ME access to main memory */
	li	$k1, -1
	sw	$k1, 0x04($k0)		/* clear the NMI flags */
	sync
	/* open the memory and hardware register protection to the ME */
	li	$k0, 0xbc000000
1:	sw	$k1, 0($k0)
	addiu	$k0, $k0, 4
	li	$at, 0xbc000048
	bne	$k0, $at, 1b
	nop
	sync
	li	$k0, 0x30000000		/* Status: CU0, CU1; ERL and BEV off */
	mtc0	$k0, $12
	sync
	la	$k0, me_entry
	li	$k1, 0x80000000		/* kernel segment */
	or	$k0, $k0, $k1
	jr	$k0
	nop
me_reset_end:

/*
 * On the ME. Its caches may still hold lines of whatever ran there before
 * (the firmware, or the previous px68k after a push): drop them before the
 * first cached access, then run me_main (psp/me.c) on our stack, and halt
 * when it returns.
 */
	.globl	me_entry
	.ent	me_entry
me_entry:
	move	$t0, $0
	li	$t1, 8192
1:	cache	0x04, 0($t0)		/* I: index invalidate */
	cache	0x04, 0($t0)
	cache	0x14, 0($t0)		/* D: index writeback invalidate */
	cache	0x14, 0($t0)
	addiu	$t0, $t0, 64
	bne	$t0, $t1, 1b
	nop
	sync
	la	$t0, me_boot
	lui	$t1, 0x4000		/* uncached */
	or	$t0, $t0, $t1
	lw	$sp, 0($t0)
	lw	$gp, 4($t0)
	la	$t9, me_main
	jalr	$t9
	nop
2:	.word	0x70000000		/* halt */
	b	2b
	nop
	.end	me_entry
