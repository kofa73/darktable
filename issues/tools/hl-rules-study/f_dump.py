# f_dump.py <plan.json> <out root> <names comma list | all> [experiments]
# Phase 3 dumps for rule F: runs the patched darktable-cli (dump.patch, GD_DUMP) at
# reduced resolution ("full resolution" off) on the clipped frame for each experiment
# value (default 7, 10, 9: hue from measured channels / continuous border / opposed's
# estimate as hue, each with the hue estimate in fully clipped), and once on the truth
# frame for its converted input. Writes <out root>/<name>/x<N>/ and .../truth/.
# Environment: DT (darktable-cli, default the dt-build copy), EV2_THREADS.
import sys, os, json, subprocess
DT = os.environ.get('DT', '/tmp/hl-rules-study/dt-build/bin/darktable-cli')
PY = '/tmp/claude-gd-artefacts/venv/bin/python'
SIDE = '/tmp/claude-gd-artefacts/scripts/make_sidecar.py'
E0 = 0.7


def run(raw, xmp_args, ddir, synth, cfg, ev):
    os.makedirs(ddir, exist_ok=True)
    xmp = os.path.join(ddir, 'side.xmp')
    subprocess.run([PY, SIDE, xmp] + xmp_args + ['--ev', str(ev), '--black', '0', '--no-comp'], check=True)
    env = dict(os.environ, GD_DUMP=ddir)
    env.pop('HL_SYNTH_EV', None)
    if synth is not None:
        env['HL_SYNTH_EV'] = '%g' % synth
    out = os.path.join(ddir, 'out.jpg')
    th = ['--threads', os.environ['EV2_THREADS']] if os.environ.get('EV2_THREADS') else []
    r = subprocess.run([DT, raw, xmp, out, '--width', '300', '--height', '300', '--core', '--configdir', cfg,
                        '--disable-opencl'] + th, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not os.path.exists(os.path.join(ddir, 'meta.txt')):
        raise RuntimeError('no dump in %s: %s' % (ddir, r.stdout[-1500:]))
    for f in (out, xmp):
        if os.path.exists(f):
            os.remove(f)


def main():
    plan = json.load(open(sys.argv[1]))
    root, names = sys.argv[2], sys.argv[3]
    exps = [int(x) for x in sys.argv[4].split(',')] if len(sys.argv) > 4 else [7, 10, 9]
    cfg = os.environ.get('SYNTH_CFG', '/tmp/hlc-eval/cfg')
    sel = plan if names == 'all' else [e for e in plan if e['name'] in names.split(',')]
    for e in sel:
        wd = os.path.join(root, e['name'])
        synth = e['k'] if e['kind'] == 'synth' else None
        for x in exps:
            d = os.path.join(wd, 'x%d' % x)
            if not os.path.exists(os.path.join(d, 'rho.f32')):
                run(e['clipped'], ['6', '--wb', '4', '--exp', str(x)], d, synth, cfg, E0 - e['k'])
        d = os.path.join(wd, 'truth')
        if not os.path.exists(os.path.join(d, 'in.f32')):
            run(e['ref'], ['6', '--wb', '4', '--exp', '7'], d, None, cfg, E0)
        print('dumped', e['name'], flush=True)


if __name__ == '__main__':
    main()
