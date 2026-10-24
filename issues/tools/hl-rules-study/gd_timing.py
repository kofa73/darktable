# gd_timing.py <out.json> <workdir>...: issue solver-accuracy-and-speed. Wall time of the highlights
# module (darktable-cli -d perf, "processed `highlights' on CPU") and gradient domain's CG iteration
# counts, one job at a time with all threads, for "continuous border, hue estimate in fully clipped",
# write-back "joint, any channel clipped, capped", at full and at reduced resolution. Extra
# environment for the renders in GT_ENV ("A=1 B=2"); GT_TAG names the variant in the output;
# GT_MODULEDIR selects another plugin directory.
# Run on an idle machine: other load distorts the times.
import sys, os, json, re, subprocess, tempfile, shutil
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

THREADS = os.environ.get('GT_THREADS', '12')
TAG = os.environ.get('GT_TAG', 'base')
EXTRA = dict(kv.split('=', 1) for kv in os.environ.get('GT_ENV', '').split())
VARIANTS = {'full': ['6', '--full', '--wb', '4', '--exp', '10'], 'reduced': ['6', '--wb', '4', '--exp', '10']}
out = json.load(open(sys.argv[1])) if os.path.exists(sys.argv[1]) else {}
tmp = tempfile.mkdtemp(dir='/tmp/hl-rules-study/scratch')
for wd in sys.argv[2:]:
    name = os.path.basename(wd.rstrip('/'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    for v, args in VARIANTS.items():
        xmp = os.path.join(tmp, 'gt.xmp')
        subprocess.run([ev2.PY, '/tmp/hl-rules-study/scripts/make_sidecar2.py', xmp] + args
                       + ['--ev', str(ev2.E0 - sc['k']), '--black', '0', '--no-comp'], check=True)
        env = dict(os.environ, OMP_WAIT_POLICY='passive', OMP_NUM_THREADS=THREADS, **EXTRA)
        r = subprocess.run(['/tmp/hl-rules-study/dt-build/bin/darktable-cli', sc['clipped'], xmp, os.path.join(tmp, 'gt.pfm'),
                            '--width', '1600', '--height', '1600', '--icc-type', 'LIN_REC2020', '--core', '--configdir',
                            '/tmp/hl-rules-study/runs/allpairs/cfg0', '--disable-opencl', '--threads', THREADS, '-d', 'perf']
                           # GT_MODULEDIR: another plugin directory (e.g. the multigrid port's)
                           + (['--moduledir', os.environ['GT_MODULEDIR']] if os.environ.get('GT_MODULEDIR') else []),
                           env=env, capture_output=True, text=True)
        t = re.search(r"took ([0-9.]+) secs \(([0-9.]+) CPU\) \[export\] processed `highlights'", r.stdout + r.stderr)
        g = re.search(r"grid: (\d+) clipped.*?CG iterations hue (\d+)/(\d+)/(\d+), channels (\d+)/(\d+)/(\d+)", r.stdout + r.stderr)
        res = dict(wall=float(t.group(1)) if t else None, cpu=float(t.group(2)) if t else None,
                   clipped=int(g.group(1)) if g else None,
                   hue_iter=[int(x) for x in g.groups()[1:4]] if g else None,
                   chan_iter=[int(x) for x in g.groups()[4:7]] if g else None)
        out.setdefault(name, {})['%s %s' % (TAG, v)] = res
        print('%-40s %-8s %-8s highlights %7s s wall %8s s CPU, clipped %s, hue it %s, channel it %s' % (
            name[:40], TAG, v, res['wall'], res['cpu'], res['clipped'], res['hue_iter'], res['chan_iter']), flush=True)
        json.dump(out, open(sys.argv[1], 'w'), indent=1)
# darktable-cli also leaves files next to the sidecar
shutil.rmtree(tmp)
