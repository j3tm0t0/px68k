# 68000 core speed on the PSP: measurements, options, design

Phase 1 of the "CPU-bound games" work: where the time of PROF_CPU goes,
what was changed, and whether a new core (hand-written MIPS interpreter or
dynamic recompiler) is worth it.

**Summary.**  On hardware the C68K core alone runs a fully busy 10 MHz 68000
at **4-5x real time** (`cpubench`, section 3), i.e. ~4-5 ms per emulated
frame even when the 68000 never idles.  SION IV's 28-42 ms of PROF_CPU in
its weapon select is therefore not instruction execution: per frame it does
only ~15k instructions but ~12.2k GVRAM word writes, each of which went
through two calls of GVRAM_Write and four levels of dispatch (now one call,
section 4), plus whatever is counted in PROF_CPU without being the 68000
(section 2.3).  A new core is not the way to 2.5x for these games; see the
recommendation (section 8).

## 1. What the 68000 does (host profiles)

Host runs: the i386 SDL build in docker, deterministic (tools/c68ktest/host.sh
setup), instrumented with an opcode histogram at the dispatch, cycles per
opcode, and every memory access counted by 64 KB region and size.  SION IV
reaches its weapon select with joystick trigger 1 held in frames 3100-3115.
Per emulated frame:

| workload | 68000 insns | cycles | C68k_Exec calls | notable |
|---|---|---|---|---|
| SION IV weapon select (3D ship), frames 3500-3900 | 14.8k | 179.6k (12.1/insn) | 848 | **12.2k GVRAM word writes** |
| 超連射68K attract demo, frames 3000-3300 | 15.3k | 179.4k (11.7/insn) | 863 | 2.5k MFP GPIP polls, 0.9k GVRAM + 0.6k BG + 0.3k TVRAM word writes |
| SION IV title (no input) | 18.8k | 180.1k | 874 | 65% of insns = `btst Dn,(An); bne` polling GPIP |

SION IV weapon select, top handlers (share of instructions / of 68000
cycles): swap 6.7/2.2, bne.s 5.9/4.9, tst.w abs.l 5.6/7.4, sub.w 4.4,
add.w 4.3, bra.w 3.5, movea.l 3.3, add.l 3.3, dbf 2.6, lea d16(An) 2.5,
movem.l -(An) 2.3/**14.7**, muls 2.5/**10.8**, divs 0.4/3.8.  364 distinct
handlers, the top 50 cover 87%: ordinary compiled 3D code, no idle loop,
few I/O accesses.

Memory accesses per frame in the weapon select:

| region | access | per frame |
|---|---|---|
| GVRAM | word write | 6177 |
| GVRAM | long write, predecrement (movem.l -(An): span fills) | 2881 |
| GVRAM | long write | 133 |
| main RAM | word / long read, word / long write, byte | ~3.7k |
| I/O | all | ~130 |

## 2. Cost model

### 2.1 The core, from the PSP code

`psp-gcc -S` of c68k.c, handlers located through the label table of
c68k_ini.c:

* dispatch: 12-14 instructions (cycle counter, `BusErrHandling` test, opcode
  and jump table loads, `jr`).
* moveq 12, move.w Dn,Dm ~12, bne.s 10-17, move.w (An)+,Dn 19 + a call of
  `cpu_readmem24_word` (13 on its RAM fast path) before this work.
* weapon select, weighted by the histogram: ~31-35 MIPS instructions per
  68000 instruction including dispatch and RAM accesses, ~0.5 M per frame.

### 2.2 GVRAM writes, before this work

`cpu_writemem24_word` (RAM test fails) -> `cpu_writemem24_word_slow` ->
`wm_cnt` (range tests) -> `GVRAM_Write`, then `wm_main` -> `wm_cnt` ->
`GVRAM_Write` again: ~100 instructions and 6 calls per word; a
`movem.l -(An)` long went through 4 more levels.  ~12.2k words per frame:
~1.2-1.5 M instructions, 2-3 times the core proper, plus, since the GE
65536 colour work, a `GE_G16Write` (~40 cycles) per changed byte.

### 2.3 Not the 68000 but counted in PROF_CPU

* PROF_CPU is wall time around each C68k_Exec call: a higher priority thread
  (sound synthesis) preempting the emulator thread inside it is counted.
* With `prof on`, each of the ~850 C68k_Exec calls per frame is wrapped in
  two `sceKernelGetSystemTimeLow` system calls.
* Worth one comparison on hardware: total frame time of the weapon select
  with prof off vs on.

## 3. Hardware calibration (`cpubench`)

Debug command `cpubench <prog> <Mcycles> [slice]`: runs a loop in a corner
of main RAM for N million 68000 cycles in C68k_Exec calls of `slice` cycles
(default 200 as the emulator), WLAN paused, and logs us per million 68000
cycles (100000 us = a real 10 MHz 68000).  CPU state, RAM and bus error
flags are restored afterwards.

* prog 0: register-only loop (moveq/add/lsl/eor/addq/cmp/beq/move/dbra)
* prog 1: RAM loop (move.l (a0)+, move.w (a1)+, move.b d16(a0), move.w d16(a6), and.w #, lsr, dbra)
* prog 2: `btst #7,$e88001; bra` (MFP GPIP poll)

333 MHz, us per 1M 68000 cycles (x real time):

| build | prog 0 | prog 1 | prog 2 | prog 1, slice 100000 |
|---|---|---|---|---|
| base (psp-tuning) | 28082 (3.56x) | 23469 (4.26x) | 27076 (3.69x) | 21892 (4.56x) |
| + cycle counter in a register, GPIP memo, memory still by calls (`-DC68K_CALL_RAM`) | 24824 (4.02x) | **48233 (2.07x)** | 24949 (4.00x) | 46624 (2.14x) |
| + RAM fast paths inlined (committed) | 25075 (3.98x) | 20362 (4.91x) | 25825 (3.87x) | 18874 (5.29x) |

* The core is 4-5x faster than a real 68000 even when busy: 180k cycles
  (one 31 kHz frame) cost ~4-5 ms.
* Slice overhead (200 vs 100000 cycles per call): ~7%.
* The call-only variant doubles the RAM loop on hardware although it runs
  fewer instructions than the base: code layout / cache effects on the
  Allegrex are large and not predictable from instruction counts.  Every
  change to the core needs a hardware run.

## 4. Done in phase 1 (all exact)

| change | check |
|---|---|
| `cpubench` debug command | - |
| MFP GPIP: its three integer divisions remembered for the values they were computed from | 超連射68K host screen hashes identical (3300 frames) |
| C68K: cycle counter in a register, synced to `CPU->ICount` around calls that leave the core | fuzz.sh identical |
| C68K: main RAM fast paths inlined into the handlers (core text 196 -> 368 KB) | fuzz.sh identical; hardware above |
| GVRAM word writes: `GVRAM_WriteWord` (65536 colour layouts in one store, GE hooks once per word per gecomp.h; 16/256 colour 512 dot: the high byte only marks line 1023, as GVRAM_Write does); mem_wrap.c calls it directly for word and long writes | gvword.sh (vs GVRAM_Write, host and -DPSP hooks), fuzz.sh, SION IV weapon select host hashes |

| C68K: idle loop skip for MFP GPIP polls (`btst #n,$e88001` / `btst Dn,(An)` + `Bcc.s` back) | fuzz.sh with the harness generating these polls (~127k skips per idle run); 超連射68K, SION IV host hashes |

The GVRAM path per word is now ~40 instructions and 3 calls (long writes:
~60 for both words instead of ~230).

Hardware results (prof off):

* SION IV weapon select: psp-tuning + GVRAM word path only 56.5 / 56.3 ms,
  + inlined core 53.9 / 54.8 ms; merged with the GE 65536 colour work:
  50.4 ms.
* 超連射68K `benchf 3000 300 1`, one part reverted at a time from the merge
  (16.89 ms): core inline reverted 15.93 (the inlined core costs it ~1 ms,
  I-cache), GPIP memo reverted 17.88, GVRAM word path reverted 17.44;
  + GPIP idle skip 15.68.

The inlined core helps SION IV (~2 ms) and costs 超連射68K (~1 ms): code
placement matters more than instruction counts on the Allegrex.  Variants
being measured: handlers reordered hot-first (from host profiles of the
three games), with the RAM fast paths inlined everywhere / only in the hot
handlers / nowhere (scratch tooling: reorder of c68k_op.c, `C68K_INL` per
region).

## 5. Remaining incremental options (a)

1. GVRAM writes straight from the core (a GVRAM test in the write macros
   before calling mem_wrap.c): saves ~2 call levels per word; only if the
   hardware still shows the GVRAM path.
2. Flags as locals like the cycle counter (~3-4 instructions per 68000
   instruction).  Risky for the code size / layout effects seen above.
3. `BusErrHandling` test per dispatch (~4 instructions): could move into the
   slow memory paths, with care for when the exception is taken.

## 6. Option (b): hand-written MIPS interpreter

A generator (Python) emitting Allegrex assembly for the ~1700 handlers, in
the style of Cyclone (ARM): CPU base, PC, cycle counter, flags, opcode,
jump table base and MEM base pinned in registers; dispatch in ~6
instructions; EA decoding and RAM fast paths inline; shared tails.

* Expected: ~2x on the core proper (dispatch 13 -> 6, ALU handlers ~12 -> ~5,
  RAM access ~14 -> ~7).  Nothing for the device paths.
* Effort: 3-5 weeks, including a test bed running the asm core against C68K
  (mipsel Linux docker + qemu-user, plain MIPS32r2 code).
* Risk: medium; mechanical and fuzzable, but the whole ISA is redone.

## 7. Option (c): dynamic recompiler (68000 -> Allegrex)

### Blocks and the code cache
* Block = 68000 code from an entry PC up to and including the first
  control flow instruction (Bcc, BRA, BSR, JMP, JSR, RTS, RTE, DBcc,
  TRAP, ...) or SR-changing instruction, or a cap (e.g. 32 instructions).
  Only code in main RAM and IPL ROM is compiled; anything else runs in C68K.
* Code cache: a static buffer (e.g. 1 MB in .bss; the heap is tight, so
  PSP_HEAP_SIZE_KB must give it up explicitly), flushed whole when full;
  `sceKernelDcacheWritebackRange` + `sceKernelIcacheInvalidateRange` per
  emitted block.  Lookup: hash of PC (64K entries x 8 bytes = 512 KB) or a
  two-level page table for RAM.
* Only the hot, simple instructions are translated (MOVE/MOVEA/MOVEQ,
  ADD/SUB/CMP/AND/OR/EOR/NOT/NEG/CLR/TST/EXT/SWAP, ADDQ/SUBQ, LEA/PEA,
  shifts by immediate, Bcc/BRA/BSR/DBcc/JSR/RTS, MOVEM, MULS/MULU); the rest
  ends the block and is interpreted (C68K needs a "run one instruction" entry).

### Exact timing
* Interrupts are only taken at C68k_Exec entry and after SR-changing
  instructions (RET_INT), as now: blocks end after any SR change.
* C68K checks `ICount > 0` before each instruction.  A block of k
  instructions with static cycles c1..ck is equivalent iff
  `ICount - (c1 + ... + c(k-1)) > 0` at entry: test that once and run C68K
  for the rest of the slice otherwise.  Instructions with data-dependent
  cycles (shifts by register, MULx/DIVx, DBcc exit) end their block so the
  entry test needs only static sums.
* Device writes may clear ICount (IRQH_Int) and bus errors set
  BusErrHandling, only in the slow memory paths: compiled code tests them
  after a slow path call only and leaves the block after that instruction.
* The 200-cycle slices stay (MFP/RTC timers, DMA, raster timing between
  slices); a block cannot run past a slice end, so blocks gain less than
  usual from long straight runs, but stay exact.

### Self-modifying code / code loading
* A byte per 256-byte page of RAM: "contains compiled code".  The RAM write
  fast path tests it (3 instructions on every RAM write); a write to such a
  page invalidates the page's blocks (and leaves the current block if it is
  among them).  DMA (dmac.c) and disk loads into RAM take the same check.

### Expected gain and effort
* Core proper: 3-6x (registers cached across a block, flags only when used,
  no dispatch).  Device paths unchanged.
* Effort: 6-10 weeks to robust (code generator, register allocation, flag
  liveness, SMC, fallbacks, qemu-mipsel test bed); cache maintenance bugs
  that only show on hardware are the main risk.

## 8. Recommendation

Do not start (b) or (c) now.  The core proper already runs a busy 68000 at
4-5x real time on hardware; at the 18 ms frame budget it uses ~4-5 ms.
Even a perfect core could not bring SION IV's 28-42 ms under budget: the
rest is the GVRAM write path (changed in this phase), the GE's per-word
work, and time that PROF_CPU attributes to the 68000 without it being the
68000 (section 2.3).  Next steps:

1. Hardware: SION IV weapon select with this branch (GVRAM word path),
   prof on and off, to see what is left.
2. If the GVRAM / GE path still dominates: item 2 of section 5 and making
   GE_G16Write cheaper per word (GE side).
3. (done: GPIP poll idle skip.)
4. Revisit (b) only if a game is found whose PROF_CPU, after the above, is
   really instruction-bound (`cpubench`-like rates times its cycle count
   explain its time); (b) is the better effort/gain point, (c) only if (b)
   is not enough.
