| RTCBENCH.X: how fast the emulator really runs, timed with the RTC.
|
| Benchmarks like SI time the MPU with the MFP timers, which an emulator
| advances with the emulated cycles, so they always report the emulated
| clock.  The RTC follows the host clock, so counting the cycles run in
| five RTC seconds gives the real speed.  Run it with the emulator's
| speed limit off (px68k: No Wait Mode).
|
| Each test runs blocks of 65536 iterations of a loop of K cycles
| (68000 timings, no wait states) and reads the RTC (IOCS _TIMEGET)
| between blocks.  ratio = blocks * K * 65536 / (10 MHz * 5 s).
|
| Build: m68k-linux-gnu-as -m68000 -o r.o rtcbench.s
|        m68k-linux-gnu-objcopy -O binary -j .text r.o rtcbench.bin
|        mkx.py rtcbench.bin RTCBENCH.X

	.text
start:
	pea	title(%pc)
	.short	0xff09			| DOS _PRINT
	addq.l	#4,%sp

	pea	name_reg(%pc)
	.short	0xff09
	addq.l	#4,%sp
	lea	block_reg(%pc),%a2
	moveq	#10,%d7			| dbra: 10 cycles
	bsr	measure

	pea	name_ram(%pc)
	.short	0xff09
	addq.l	#4,%sp
	lea	block_ram(%pc),%a2
	moveq	#22,%d7			| move.l (a0),d2: 12, dbra: 10
	bsr	measure

	.short	0xff00			| DOS _EXIT

| a2: block, d7: cycles per iteration
measure:
	bsr	rtc_sec
	move.b	%d0,%d6
1:	bsr	rtc_sec			| start on a second boundary
	cmp.b	%d6,%d0
	beq	1b
	move.b	%d0,%d6
	moveq	#0,%d5			| blocks
	moveq	#0,%d4			| seconds
2:	jsr	(%a2)
	addq.l	#1,%d5
	bsr	rtc_sec
	cmp.b	%d6,%d0
	beq	2b
	move.b	%d0,%d6
	addq.w	#1,%d4
	cmp.w	#5,%d4
	bne	2b
	| ratio * 1000 = (blocks * K) << 16 / 50000
	move.l	%d5,%d0
	mulu	%d7,%d0
	swap	%d0
	clr.w	%d0
	move.l	%d0,%d1
	| 32/16 division in two steps (the quotient may exceed 16 bits)
	clr.w	%d1
	swap	%d1
	divu	#50000,%d1		| high word
	move.l	%d1,%d2			| remainder in the high word
	move.w	%d0,%d2
	divu	#50000,%d2		| low word
	swap	%d1
	move.w	%d2,%d1			| d1 = ratio * 1000
	move.l	%d1,%d3
	lea	buf_end(%pc),%a0
	clr.b	-(%a0)
	move.b	#0x0a,-(%a0)
	move.b	#0x0d,-(%a0)
	move.b	#'x',-(%a0)
	move.b	#' ',-(%a0)
	moveq	#3,%d2
3:	bsr	digit
	subq.w	#1,%d2
	bne	3b
	move.b	#'.',-(%a0)
4:	bsr	digit
	tst.l	%d1
	bne	4b
	pea	(%a0)
	.short	0xff09
	addq.l	#4,%sp
	| MHz equivalent = ratio * 10
	lea	buf_end(%pc),%a0
	clr.b	-(%a0)
	move.b	#0x0a,-(%a0)
	move.b	#0x0d,-(%a0)
	move.b	#')',-(%a0)
	move.b	#'z',-(%a0)
	move.b	#'H',-(%a0)
	move.b	#'M',-(%a0)
	move.b	#' ',-(%a0)
	move.l	%d3,%d1			| ratio * 1000 = MHz * 100
	moveq	#2,%d2
5:	bsr	digit
	subq.w	#1,%d2
	bne	5b
	move.b	#'.',-(%a0)
6:	bsr	digit
	tst.l	%d1
	bne	6b
	move.b	#'(',-(%a0)
	move.b	#' ',-(%a0)
	move.b	#' ',-(%a0)
	pea	(%a0)
	.short	0xff09
	addq.l	#4,%sp
	rts

| d1 /= 10, the remainder as a digit at -(a0)
digit:
	move.l	%d1,%d0
	clr.w	%d0
	swap	%d0
	divu	#10,%d0
	move.l	%d0,%d4
	move.w	%d1,%d4
	divu	#10,%d4
	swap	%d0
	move.w	%d4,%d0			| quotient
	swap	%d4
	add.b	#'0',%d4
	move.b	%d4,-(%a0)
	move.l	%d0,%d1
	rts

| d0.b: RTC seconds (BCD)
rtc_sec:
	movem.l	%d1-%d7/%a0-%a6,-(%sp)
	moveq	#0x56,%d0		| IOCS _TIMEGET
	trap	#15
	movem.l	(%sp)+,%d1-%d7/%a0-%a6
	rts

block_reg:
	move.w	#0xffff,%d1
1:	dbra	%d1,1b
	rts

block_ram:
	lea	cell(%pc),%a0
	move.w	#0xffff,%d1
1:	move.l	(%a0),%d2
	dbra	%d1,1b
	rts

	.even
cell:	.long	0
title:	.ascii	"RTCBENCH: real speed against a 10 MHz 68000 (RTC timed, 5 s per test)\r\n\0"
name_reg:	.ascii	"register loop (dbra)       : \0"
name_ram:	.ascii	"main RAM loop (move.l/dbra): \0"
	.even
buf:	.space	32
buf_end:
