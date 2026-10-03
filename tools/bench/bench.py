#!/usr/bin/env python3
"""Speed of the optimizations of this branch, one group at a time, without
ROMs or disk images: synthetic 68000 programs on the core (core.c), the MFP
(periph.c), fmgen's OPM (fm.cpp) and the CPU-path line renderer (rend.c).

  tools/bench/bench.py [--rounds N] [--secs S] [--only v1,v2] [--json out]

Every variant is the HEAD sources with one group taken back, by a build
macro or by older revisions of the files the group changed; "base" is the
tree before any of them.  The drivers are always HEAD's tools/bench.  A
binary whose inputs are the same as HEAD's is not run again for a variant.
The variants run in a shuffled order each round, every run of a variant
right after a run of HEAD's binary; the gains are the medians over the
rounds of these pairs.  Run from the
repository root on a 32-bit x86 target: CC='gcc -m32' CXX='g++ -m32' on
x86_64 (the 68000 core keeps host pointers in 32 bits).  The table goes to
stdout and, on GitHub Actions, to the job summary.
"""
import argparse, concurrent.futures, hashlib, json, os, random, shlex, shutil, statistics, subprocess, sys

ROOT = os.getcwd()
BENCH = os.path.join(ROOT, 'tools', 'bench')
OUT = os.path.join(ROOT, 'build', 'bench')
BASE_REV = '7b29341'	# master + the build fixes, before any optimization
ORDER = 'c68korder'	# the file made by tools/c68ktest/c68korder.py hot995.txt 1 1

# name, description, {path: rev}, extra cflags, extra source (shim), binaries it changes
ALL = {'core', 'periph', 'fm', 'rend'}
VARIANTS = [
    ('base', 'master + build fixes (7b29341)', {'*': BASE_REV}, '', None, ALL),
    ('head', 'this branch (generic build)', {}, '', None, ALL),
    # C68K
    ('no_idle', 'no idle loop skips (tst/cmp, GPIP polls)', {}, '-DC68K_NO_IDLE', None, {'core'}),
    ('no_direct', 'mem_wrap.c through the handler pointers (C68K_NO_DIRECT_MEM)', {}, '-DC68K_NO_DIRECT_MEM', None,
     {'core'}),
    ('no_regicount', 'cycle counter in CPU->ICount (C68K_NO_REG_ICOUNT)', {}, '-DC68K_NO_REG_ICOUNT', None, {'core'}),
    ('core_master', 'C68K as on master (no idle skips, handler pointers, CPU->ICount), mem_wrap.c without '
     'GVRAM_WriteWord', {'x68k/mem_wrap.c': '59c56ec^'}, '-DC68K_NO_IDLE -DC68K_NO_DIRECT_MEM -DC68K_NO_REG_ICOUNT',
     None, {'core'}),
    ('no_gvword', 'mem_wrap.c without GVRAM_WriteWord', {'x68k/mem_wrap.c': '59c56ec^'}, '', None, {'core'}),
    ('psp_inline', 'PSP only: RAM fast paths inlined (C68K_INLINE_RAM)', {}, '-DC68K_INLINE_RAM', None, {'core'}),
    ('psp_order', 'PSP only: hot handlers first (c68korder.py hot995.txt 1 1)', {'m68000/c68k_op.c': ORDER}, '',
     None, {'core'}),
    ('psp_core', 'the PSP\'s core: both', {'m68000/c68k_op.c': ORDER}, '-DC68K_INLINE_RAM', None, {'core'}),
    # peripherals
    ('psp_gpipmemo', 'PSP only: GPIP reads remember their divisions (MFP_GPIP_MEMO)', {}, '-DMFP_GPIP_MEMO', None,
     {'periph'}),
    ('old_mfptimer', 'MFP timers per prescaler tick', {'x68k/mfp.c': 'b2fc9d4^'}, '', None, {'periph'}),
    ('old_rtc', 'RTC_Timer out of line', {'x68k/rtc.c': '8323f7f^', 'x68k/rtc.h': '8323f7f^'}, '', None, {'periph'}),
    ('old_adpcm', 'ADPCM_PreUpdate out of line, with its divisions', {'x68k/adpcm.c': '3be6581^', 'x68k/adpcm.h': '3be6581^'},
     '', None, {'periph'}),
    ('psp_fmtables', 'PSP only: fmgen 16-bit sine/level tables (FMGEN_TABLE16)', {}, '-DFMGEN_TABLE16', None, {'fm'}),
    ('old_fmtimer', 'fmgen: Timer::Count without the inline fast path',
     {'fmgen/fmtimer.cpp': '3be6581^', 'fmgen/fmtimer.h': '3be6581^'}, '', None, {'fm'}),
    # rendering
    ('old_bg', 'bg.c as on master', {'x68k/bg.c': BASE_REV}, '', None, {'rend'}),
    ('old_gvram', 'gvram.c as on master (no 4-page pass)', {'x68k/gvram.c': BASE_REV}, '', 'multi_shim.c', {'rend'}),
    ('no_multi', 'gvram.c decoders, no 4-page pass (Grp_DrawLine4Multi)', {'x68k/gvram.c': 'cae673e'}, '', 'multi_shim.c', {'rend'}),
    ('old_windraw', 'windraw.c compositing as on master (WD_* macros)', {'x11/windraw.c': 'e590392^'}, '', None, {'rend'}),
    ('psp_grp4opaq', 'PSP only: an opaque 16-colour page through the pair table (GRP4_OPAQ_TABLE)', {}, '-DGRP4_OPAQ_TABLE',
     None, {'rend'}),
]

# group, variant with it, variant without it, tests that show it
CORE = ['reg', 'ram', 'gpip_read', 'mix', 'gvram_long64k']
GROUPS = [
    ('idle loop skips', 'head', 'no_idle', ['idle_gpip_abs', 'idle_gpip_dn', 'idle_tst']),
    ('direct mem_wrap calls', 'head', 'no_direct', CORE + ['gvram_movem64k']),
    ('cycle counter in a register', 'head', 'no_regicount', CORE),
    ('mem_wrap.c RAM fast paths', 'core_master', 'base', CORE),
    ('GVRAM_WriteWord', 'head', 'no_gvword', ['gvram_long64k', 'gvram_movem64k', 'gvram_word64k', 'gvram_word16']),
    ('PSP only: RAM fast paths inlined', 'psp_inline', 'head', CORE),
    ('PSP only: hot handlers first', 'psp_order', 'head', CORE),
    ('PSP only: both (the PSP\'s core)', 'psp_core', 'head', CORE + ['idle_gpip_abs', 'gvram_movem64k']),
    ('MFP timers per underflow', 'head', 'old_mfptimer', ['mfp_timer']),
    ('PSP only: GPIP read memo', 'psp_gpipmemo', 'head', ['gpip_read1', 'gpip_read10']),
    ('RTC_Timer inline', 'head', 'old_rtc', ['rtc_timer']),
    ('ADPCM_PreUpdate inline', 'head', 'old_adpcm', ['adpcm_pre']),
    ('PSP only: fmgen 16-bit tables', 'psp_fmtables', 'head', ['opm_mix1', 'opm_mix64']),
    ('fmgen Timer::Count fast path', 'head', 'old_fmtimer', ['opm_count']),
    ('bg.c rewrite', 'head', 'old_bg', ['bg256', 'g16x4']),
    ('gvram.c decoders + 4-page pass', 'head', 'old_gvram', ['g16x1', 'g16x4', 'g16x4_tr', 'g256x2', 'g64k', 'bg256']),
    ('4-page pass (Grp_DrawLine4Multi)', 'head', 'no_multi', ['g16x4']),
    ('PSP only: opaque 16-colour page by pair table', 'psp_grp4opaq', 'head', ['g16x1', 'bg256', 'g16x4']),
    ('windraw.c wd_dst compositing', 'head', 'old_windraw', ['bg256', 'g16x1', 'g16x4', 'g16x4_tr', 'g256x2', 'g64k', 'text768']),
    ('all of it (generic build)', 'head', 'base', None),
]

# done in one binary: the new way, the old way (copies of code that cannot be called alone)
PAIRS = [
    ('PSP only: clk_next without a division per line', 'clknext_step', 'clknext_div'),
]

# The core is an interpreter in one huge function: how gcc happens to lay
# out its handlers moved single-flag comparisons by up to 20% either way on
# x86.  Every core variant is built with these alignments and the gains of
# a group are taken over all of them (and shown per layout).
LAYOUTS = [
    '',
    '-falign-functions=64 -falign-jumps=32 -falign-loops=32 -falign-labels=16',
    '-fno-align-functions -fno-align-jumps -fno-align-loops -fno-align-labels',
]

DIRS = ['m68000', 'x68k', 'x11', 'win32api', 'fmgen']
CC = shlex.split(os.environ.get('CC', 'gcc'))
CXX = shlex.split(os.environ.get('CXX', 'g++'))
CFLAGS = shlex.split(os.environ.get('BENCH_CFLAGS', '-O2 -fno-strict-aliasing'))


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def git_show(rev, path):
    return subprocess.run(['git', 'show', '%s:%s' % (rev, path)], check=True, capture_output=True).stdout


def prepare(name, files):
    """sources of a variant under OUT/name/src"""
    src = os.path.join(OUT, name, 'src')
    sh(['rm', '-rf', src])
    os.makedirs(src)
    rev = files.get('*', 'HEAD')
    tar = subprocess.run(['git', 'archive', rev] + DIRS, check=True, capture_output=True).stdout
    sh(['tar', 'xf', '-', '-C', src], input=tar)
    for path, r in files.items():
        if r == ORDER:
            f = os.path.join(src, path)
            sh([sys.executable, os.path.join(ROOT, 'tools', 'c68ktest', 'c68korder.py'),
                os.path.join(ROOT, 'tools', 'c68ktest', 'hot995.txt'), '1', '1', f, f], stdout=subprocess.DEVNULL)
        elif path != '*':
            open(os.path.join(src, path), 'wb').write(git_show(r, path))
    return src


def build(name, src, cflags, shim, which):
    """the four binaries of a variant; returns {binary: (path, input hash)}"""
    gen = os.path.join(OUT, name, 'gen')
    os.makedirs(gen, exist_ok=True)
    sh([sys.executable, os.path.join(BENCH, 'gen_mix.py'), os.path.join(src, 'm68000', 'c68k_ini.c'),
        os.path.join(ROOT, 'tools', 'c68ktest', 'hot995.txt'), os.path.join(gen, 'mixops.h')])
    sh([sys.executable, os.path.join(BENCH, 'wdline.py'), os.path.join(src, 'x11', 'windraw.c'),
        os.path.join(gen, 'wdline.c')])
    inc = ['-I' + os.path.join(src, d) for d in ('x11', 'x68k', 'fmgen', 'win32api', 'm68000')] + ['-I' + gen]
    s = lambda *p: [os.path.join(src, x) for x in p]
    b = lambda *p: [os.path.join(BENCH, x) for x in p]
    bins = {
        'core': (CC, b('core.c') + s('m68000/c68k.c', 'm68000/m68000.c', 'x68k/mem_wrap.c', 'x68k/gvram.c') +
                 [os.path.join(gen, 'mixops.h')]),
        'periph': (CC, b('periph.c') + s('x68k/mfp.c', 'x68k/rtc.c', 'x68k/adpcm.c')),
        'fm': (CXX, b('fm.cpp') + s('fmgen/opm.cpp', 'fmgen/fmgen.cpp', 'fmgen/fmtimer.cpp')),
        'rend': (CC, b('rend.c') + [os.path.join(gen, 'wdline.c')] +
                 s('x68k/bg.c', 'x68k/gvram.c', 'x68k/tvram.c', 'x68k/palette.c', 'x68k/crtc.c') +
                 (b(shim) if shim else [])),
    }
    out = {}
    for bn, (cc, files) in bins.items():
        if bn not in which:
            continue
        for li, lay in enumerate(LAYOUTS if bn == 'core' else ['']):
            flags = CFLAGS + shlex.split(lay) + shlex.split(cflags)
            h = hashlib.sha256(' '.join(cc + flags).encode())
            hdrs = []
            for d in ('x11', 'x68k', 'fmgen', 'win32api', 'm68000'):
                hdrs += sorted(os.path.join(src, d, f) for f in os.listdir(os.path.join(src, d))
                               if f.endswith('.h') or f == 'c68k_op.c' or f == 'c68k_ini.c')
            for f in files + hdrs:
                h.update(open(f, 'rb').read())
            key = bn if bn != 'core' else 'core#%d' % li
            exe = os.path.join(OUT, name, key.replace('#', '_'))
            sh(cc + flags + ['-w'] + inc + ['-o', exe] + [f for f in files if not f.endswith('.h')] + ['-lm'])
            out[key] = (exe, h.hexdigest())
    return out


# the same addresses in every run (heap alignment changed the timing of the mix)
NORAND = ['setarch', os.uname().machine, '-R']
if not shutil.which('setarch') or subprocess.run(NORAND + ['true'], capture_output=True).returncode:
    NORAND = []		# e.g. under qemu-user


def run(exe, secs):
    r = subprocess.run(NORAND + [exe, str(secs)], check=True, capture_output=True, text=True)
    vals, hashes = {}, {}
    for line in r.stdout.splitlines():
        k, v = line.split()
        if k.endswith('_hash'):
            hashes[k[:-5]] = v
        else:
            vals[k] = float(v)
    return vals, hashes, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rounds', type=int, default=7)
    ap.add_argument('--secs', type=float, default=0.25)
    ap.add_argument('--only', default='')
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    only = set(a.only.split(',')) if a.only else None
    variants = [v for v in VARIANTS if not only or v[0] in only or v[0] == 'head']
    srcs = {}
    for name, desc, files, cflags, shim, which in variants:
        srcs[name] = prepare(name, files)
    print('building %d variants' % len(variants), file=sys.stderr)
    with concurrent.futures.ThreadPoolExecutor(int(os.environ.get('BENCH_JOBS', min(os.cpu_count() or 2, 4)))) as ex:
        futs = {v[0]: ex.submit(build, v[0], srcs[v[0]], v[3], v[4], v[5]) for v in variants}
        bins = {n: f.result() for n, f in futs.items()}
    # what each variant runs: the binaries that differ from HEAD's
    torun = {}
    for name, *_ in variants:
        torun[name] = [bn for bn, (exe, h) in bins[name].items()
                       if name == 'head' or h != bins['head'][bn][1]]
    # Every run of a variant's binary comes right after a run of HEAD's: the
    # gains are worked out per pair (the runner's speed drifts over minutes).
    vals = {}		# (variant, test) -> [values]
    rel = {}		# (variant, test) -> [value / HEAD's value of the run before]
    hashes = {}		# test -> {hash: [variants]}
    notes = {}
    def record(name, bn, res):
        v, h, err = res
        for k, x in v.items():
            vals.setdefault((name, k), []).append(x)
        for k, x in h.items():
            hashes.setdefault(k, {}).setdefault(x, set()).add(name)
        if err.strip():
            notes[(name, bn)] = err.strip()
        return v
    for rnd in range(a.rounds):
        order = [v[0] for v in variants if v[0] != 'head']
        random.Random(rnd).shuffle(order)
        print('round %d/%d' % (rnd + 1, a.rounds), file=sys.stderr)
        for name in order:
            for bn in torun[name]:
                hv = record('head', bn, run(bins['head'][bn][0], a.secs))
                v = record(name, bn, run(bins[name][bn][0], a.secs))
                lay = int(bn.split('#')[1]) if '#' in bn else 0
                for k, x in v.items():
                    if hv.get(k):
                        rel.setdefault((name, k), {}).setdefault(lay, []).append(x / hv[k])
    tests = []
    for (name, k) in vals:
        if k not in tests:
            tests.append(k)
    def med(name, k):
        xs = vals.get((name, k)) or vals.get(('head', k))
        return statistics.median(xs) if xs else None
    def ratios(name, k):
        """{layout: per round, the variant's value / HEAD's}, None for HEAD's binary"""
        return None if name == 'head' else rel.get((name, k))
    def gain(w, wo, k):
        """with / without per round and layout -> median gain, lowest, highest, per layout"""
        rw, ro = ratios(w, k), ratios(wo, k)
        if rw is None and ro is None:
            return None
        lays = sorted((rw or ro).keys())
        allg, per = [], []
        for l in lays:
            xs = rw[l] if rw else [1.0] * len(ro[l])
            ys = ro[l] if ro else [1.0] * len(rw[l])
            g = [1 - x / y for x, y in zip(xs, ys)]
            allg += g
            per.append(statistics.median(g) * 100)
        allg.sort()
        return statistics.median(allg) * 100, allg[0] * 100, allg[-1] * 100, per
    lines = []
    p = lines.append
    p('## px68k optimization groups, x86 (32-bit) host')
    p('')
    p('Lower values are faster.  core: us per 1M 68000 cycles; periph/fm: ns per call or per sample; '
      'rend: ns per screen line.  Each value is the best of 3 windows of a run of %.2f s per test; '
      'every run of a variant follows a run of HEAD, %d rounds.  Gain: how much less time the code takes '
      'with the optimization, median over the rounds of the paired runs, and the lowest and highest round; '
      'core tests: over the %d code layouts too (gcc alignments: default / large / none), with the median '
      'per layout.' % (a.secs, a.rounds, len(LAYOUTS)))
    p('')
    p('| group | test | with | without | gain | lowest .. highest | per layout |')
    p('|---|---|---:|---:|---:|---:|---|')
    for g, w, wo, ts in GROUPS:
        if w not in bins or wo not in bins:
            continue
        for k in (ts or tests):
            mw, mo = med(w, k), med(wo, k)
            if mw is None or mo is None:
                continue
            r = gain(w, wo, k)
            if r is None:
                p('| %s | %s | %.1f | = (same binary) | | | |' % (g, k, mw))
                continue
            gm, lo, hi, per = r
            p('| %s | %s | %.1f | %.1f | **%+.1f%%** | %+.1f%% .. %+.1f%% | %s |' % (
                g, k, mw, mo, gm, lo, hi, ' / '.join('%+.1f%%' % x for x in per) if len(per) > 1 else ''))
    for g, a1, a2 in PAIRS:
        xs, ys = vals.get(('head', a1)), vals.get(('head', a2))
        if xs and ys:
            gg = sorted(1 - x / y for x, y in zip(xs, ys))
            p('| %s | %s vs %s | %.2f | %.2f | **%+.1f%%** | %+.1f%% .. %+.1f%% | |' % (
                g, a1, a2, statistics.median(xs), statistics.median(ys),
                statistics.median(gg) * 100, gg[0] * 100, gg[-1] * 100))
    p('')
    p('<details><summary>all values (median over the runs)</summary>')
    p('')
    p('| test | ' + ' | '.join(v[0] for v in variants) + ' |')
    p('|---|' + '---:|' * len(variants))
    for k in tests:
        row = []
        for v in variants:
            row.append('%.1f' % med(v[0], k) if (v[0], k) in vals else '=')
        p('| %s | %s |' % (k, ' | '.join(row)))
    p('')
    p('Variants:')
    p('')
    for v in variants:
        p('* `%s`: %s' % (v[0], v[1]))
    p('</details>')
    p('')
    bad = {k: hs for k, hs in hashes.items() if len(hs) > 1}
    if bad:
        p('**Rendering differs between variants:** ' + '; '.join(
            '%s: %s' % (k, ' / '.join(','.join(sorted(n)) for n in hs.values())) for k, hs in bad.items()))
    else:
        p('Rendered frames and the instruction mix: identical in all variants (%d checks).' % len(hashes))
    for (name, bn), e in sorted(notes.items()):
        if name == 'head':
            p('')
            p('`%s`: %s' % (bn, e.splitlines()[-1]))
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        open(os.environ['GITHUB_STEP_SUMMARY'], 'a').write(text)
    if a.json:
        json.dump({'values': {'%s/%s' % k: v for k, v in vals.items()},
                   'ratios': {'%s/%s' % k: {str(l): x for l, x in v.items()} for k, v in rel.items()}},
                  open(a.json, 'w'), indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
