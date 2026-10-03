#!/usr/bin/env python3
"""Speed of the optimizations of this branch, one group at a time, without
ROMs or disk images: synthetic 68000 programs on the core (core.c), the MFP
(periph.c), fmgen's OPM (fm.cpp) and the CPU-path line renderer (rend.c).

  tools/bench/bench.py [--rounds N] [--secs S] [--only v1,v2] [--json out]

Every variant is the HEAD sources with one group taken back, by a build
macro or by older revisions of the files the group changed; "base" is the
tree before any of them.  The drivers are always HEAD's tools/bench.  A
binary whose inputs are the same as HEAD's is not run again for a variant.
The variants run round-robin (shuffled per round); each value is the median
over the rounds, with the spread (min-max) next to it.  Run from the
repository root on a 32-bit x86 target: CC='gcc -m32' CXX='g++ -m32' on
x86_64 (the 68000 core keeps host pointers in 32 bits).  The table goes to
stdout and, on GitHub Actions, to the job summary.
"""
import argparse, concurrent.futures, hashlib, json, os, random, shlex, shutil, statistics, subprocess, sys

ROOT = os.getcwd()
BENCH = os.path.join(ROOT, 'tools', 'bench')
OUT = os.path.join(ROOT, 'build', 'bench')
BASE_REV = '7b29341'	# master + the build fixes, before any optimization

# name, description, {path: rev}, extra cflags, extra source (shim), binaries it changes
ALL = {'core', 'periph', 'fm', 'rend'}
VARIANTS = [
    ('base', 'master + build fixes (7b29341)', {'*': BASE_REV}, '', None, ALL),
    ('head', 'this branch', {}, '', None, ALL),
    ('no_idle', 'C68K: no idle loop skips (tst/cmp, GPIP polls)', {}, '-DC68K_NO_IDLE', None, {'core'}),
    ('no_inline', 'C68K: RAM fast paths not inlined (C68K_CALL_RAM)', {}, '-DC68K_CALL_RAM', None, {'core'}),
    ('no_regicount', 'C68K: cycle counter in CPU->ICount', {}, '-DC68K_NO_REG_ICOUNT', None, {'core'}),
    ('master_order', 'C68K: master\'s handler order in c68k_op.c', {'m68000/c68k_op.c': 'f013ce9'}, '', None, {'core'}),
    ('no_direct', 'C68K: memory through the handler pointers (C68K_NO_DIRECT_MEM; no idle skips, no inlining)',
     {}, '-DC68K_NO_DIRECT_MEM', None, {'core'}),
    ('core_master', 'C68K as on master (no direct calls, ICount, master order), mem_wrap.c without GVRAM_WriteWord',
     {'m68000/c68k_op.c': 'f013ce9', 'x68k/mem_wrap.c': '59c56ec^'}, '-DC68K_NO_DIRECT_MEM -DC68K_NO_REG_ICOUNT', None, {'core'}),
    ('no_gvword', 'mem_wrap.c without GVRAM_WriteWord', {'x68k/mem_wrap.c': '59c56ec^'}, '', None, {'core'}),
    ('no_gpipmemo', 'MFP: GPIP reads without the memo', {'x68k/mfp.c': '244de43^'}, '', None, {'periph'}),
    ('old_mfptimer', 'MFP: timers per prescaler tick (and no GPIP memo)', {'x68k/mfp.c': 'b2fc9d4^'}, '', None, {'periph'}),
    ('old_rtc', 'RTC_Timer out of line', {'x68k/rtc.c': '8323f7f^', 'x68k/rtc.h': '8323f7f^'}, '', None, {'periph'}),
    ('old_adpcm', 'ADPCM_PreUpdate out of line, with its divisions', {'x68k/adpcm.c': '3be6581^', 'x68k/adpcm.h': '3be6581^'},
     '', None, {'periph'}),
    ('old_fmtables', 'fmgen: 32-bit sine/level tables', {'fmgen/fmgen.cpp': 'a82ba45^', 'fmgen/fmgen.h': 'a82ba45^'}, '', None, {'fm'}),
    ('old_fmtimer', 'fmgen: Timer::Count without the inline fast path',
     {'fmgen/fmtimer.cpp': '3be6581^', 'fmgen/fmtimer.h': '3be6581^'}, '', None, {'fm'}),
    ('old_bg', 'bg.c as on master', {'x68k/bg.c': BASE_REV}, '', None, {'rend'}),
    ('old_gvram', 'gvram.c as on master (no 4-page pass)', {'x68k/gvram.c': BASE_REV}, '', 'multi_shim.c', {'rend'}),
    ('no_multi', 'gvram.c decoders, no 4-page pass (Grp_DrawLine4Multi)', {'x68k/gvram.c': 'cae673e'}, '', 'multi_shim.c', {'rend'}),
    ('old_windraw', 'windraw.c compositing as on master (WD_* macros)', {'x11/windraw.c': 'e590392^'}, '', None, {'rend'}),
]

# group, variant with it, variant without it, tests that show it
GROUPS = [
    ('idle loop skips', 'head', 'no_idle', ['idle_gpip_abs', 'idle_gpip_dn', 'idle_tst']),
    ('RAM fast paths inlined in the core', 'head', 'no_inline', ['reg', 'ram', 'mix', 'gvram_long64k']),
    ('cycle counter in a register', 'head', 'no_regicount', ['reg', 'ram', 'mix', 'gpip_read']),
    ('hot handlers first', 'head', 'master_order', ['reg', 'ram', 'mix']),
    ('direct mem_wrap calls (+inline, +idle)', 'head', 'no_direct', ['reg', 'ram', 'mix', 'gpip_read', 'gvram_long64k']),
    ('mem_wrap.c RAM fast paths', 'core_master', 'base', ['reg', 'ram', 'mix', 'gpip_read']),
    ('GVRAM_WriteWord', 'head', 'no_gvword', ['gvram_long64k', 'gvram_movem64k', 'gvram_word64k', 'gvram_word16']),
    ('GPIP read memo', 'head', 'no_gpipmemo', ['gpip_read1', 'gpip_read10']),
    ('MFP timers per underflow', 'no_gpipmemo', 'old_mfptimer', ['mfp_timer']),
    ('RTC_Timer inline', 'head', 'old_rtc', ['rtc_timer']),
    ('ADPCM_PreUpdate inline', 'head', 'old_adpcm', ['adpcm_pre']),
    ('fmgen 16-bit tables', 'head', 'old_fmtables', ['opm_mix1', 'opm_mix64']),
    ('fmgen Timer::Count fast path', 'head', 'old_fmtimer', ['opm_count']),
    ('bg.c rewrite', 'head', 'old_bg', ['bg256', 'g16x4']),
    ('gvram.c decoders + 4-page pass', 'head', 'old_gvram', ['g16x1', 'g16x4', 'g16x4_tr', 'g256x2', 'g64k', 'bg256']),
    ('4-page pass (Grp_DrawLine4Multi)', 'head', 'no_multi', ['g16x4']),
    ('windraw.c wd_dst compositing', 'head', 'old_windraw', ['bg256', 'g16x1', 'g16x4', 'g16x4_tr', 'g256x2', 'g64k', 'text768']),
    ('all of it', 'head', 'base', None),
]

# done in one binary: the new way, the old way (copies of code that cannot be called alone)
PAIRS = [
    ('clk_next without a division per line', 'clknext_step', 'clknext_div'),
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
        if path != '*':
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
        h = hashlib.sha256(' '.join(cc + CFLAGS + shlex.split(cflags)).encode())
        hdrs = []
        for d in ('x11', 'x68k', 'fmgen', 'win32api', 'm68000'):
            hdrs += sorted(os.path.join(src, d, f) for f in os.listdir(os.path.join(src, d))
                           if f.endswith('.h') or f == 'c68k_op.c' or f == 'c68k_ini.c')
        for f in files + hdrs:
            h.update(open(f, 'rb').read())
        exe = os.path.join(OUT, name, bn)
        sh(cc + CFLAGS + shlex.split(cflags) + ['-w'] + inc + ['-o', exe] +
           [f for f in files if not f.endswith('.h')] + ['-lm'])
        out[bn] = (exe, h.hexdigest())
    return out


# the same addresses in every run (heap alignment changed the timing of the mix)
NORAND = ['setarch', os.uname().machine, '-R'] if shutil.which('setarch') else []


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
    ap.add_argument('--rounds', type=int, default=9)
    ap.add_argument('--secs', type=float, default=0.4)
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
    vals = {}		# (variant, test) -> [values]
    hashes = {}		# test -> {hash: [variants]}
    notes = {}
    for rnd in range(a.rounds):
        order = [v[0] for v in variants]
        random.Random(rnd).shuffle(order)
        print('round %d/%d' % (rnd + 1, a.rounds), file=sys.stderr)
        for name in order:
            for bn in torun[name]:
                v, h, err = run(bins[name][bn][0], a.secs)
                for k, x in v.items():
                    vals.setdefault((name, k), []).append(x)
                for k, x in h.items():
                    hashes.setdefault(k, {}).setdefault(x, set()).add(name)
                if err.strip():
                    notes[(name, bn)] = err.strip()
    # a variant's value of a test it did not run is HEAD's (same binary)
    tests = []
    for (name, k) in vals:
        if k not in tests:
            tests.append(k)
    def med(name, k):
        xs = vals.get((name, k)) or vals.get(('head', k))
        return statistics.median(xs) if xs else None
    def spread(name, k):
        xs = vals.get((name, k))
        if not xs:
            return None
        m = statistics.median(xs)
        return (max(xs) - min(xs)) / m * 100 if m else 0
    lines = []
    p = lines.append
    p('## px68k optimization groups, x86 (32-bit) host')
    p('')
    p('Lower is better.  core: us per 1M 68000 cycles; periph/fm: ns per call or per sample; '
      'rend: ns per screen line.  Median of %d rounds of %.2f s per test; spread = (max-min)/median.'
      % (a.rounds, a.secs))
    p('')
    p('| group | test | with | without | gain | spread with / without |')
    p('|---|---|---:|---:|---:|---:|')
    for g, w, wo, ts in GROUPS:
        if w not in bins or wo not in bins:
            continue
        for k in (ts or tests):
            mw, mo = med(w, k), med(wo, k)
            if mw is None or mo is None:
                continue
            sw = spread(w, k) if (w, k) in vals else spread('head', k)
            so = spread(wo, k) if (wo, k) in vals else None
            same = (wo, k) not in vals and wo != 'head'
            p('| %s | %s | %.1f | %s | %s | %s |' % (
                g, k, mw, '%.1f' % mo if not same else '= (same binary)',
                '%+.1f%%' % ((mo - mw) / mo * 100) if not same else '',
                '%.1f%% / %s' % (sw or 0, '%.1f%%' % so if so is not None else '-')))
    for g, a1, a2 in PAIRS:
        m1, m2 = med('head', a1), med('head', a2)
        if m1 is not None and m2 is not None:
            p('| %s | %s vs %s | %.2f | %.2f | %+.1f%% | %.1f%% / %.1f%% |' % (
                g, a1, a2, m1, m2, (m2 - m1) / m2 * 100, spread('head', a1), spread('head', a2)))
    p('')
    p('<details><summary>all values (median, spread)</summary>')
    p('')
    p('| test | ' + ' | '.join(v[0] for v in variants) + ' |')
    p('|---|' + '---:|' * len(variants))
    for k in tests:
        row = []
        for v in variants:
            if (v[0], k) in vals:
                row.append('%.1f (%.0f%%)' % (med(v[0], k), spread(v[0], k)))
            else:
                row.append('=')
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
        json.dump({'%s/%s' % k: v for k, v in vals.items()}, open(a.json, 'w'), indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
