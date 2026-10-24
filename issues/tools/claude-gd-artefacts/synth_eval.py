# synth_eval.py <raw> <k EV> <workdir> [methods...]
# Scores highlight reconstruction methods against ground truth made by artificial
# clipping. It needs darktable built with /tmp/claude-gd-artefacts/synth.patch.
#
# The truth is the raw rendered as it is, with highlights off. Each method render
# gets HL_SYNTH_EV=k, so rawprepare raises the raw data by k EV and clips it at the
# white level, and its exposure is k EV lower, so that it matches the truth wherever
# nothing clipped. All renders have no tone mapper and are linear Rec2020 PFM.
#
# Scored pixels ("affected") are those where the render without reconstruction
# differs from the truth by more than dE 2. Pixels that were already clipped in the
# raw (where the truth with opposed differs from the truth without) are left out.
# Writes <workdir>/scores.json, masks.npz and an sRGB PNG of every render; the PFMs
# are deleted, unless SYNTH_KEEP_PFM is set.
import sys, os, json, subprocess, numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from pfm import read_pfm
import colour

DT = '/workspace/darktable/build/bin/darktable-cli'
# parallel runs need their own config directory: darktable locks it
CFG = os.environ.get('SYNTH_CFG', '/tmp/hlc-eval/cfg')
SIDE = '/tmp/claude-gd-artefacts/scripts/make_sidecar.py'
PY = '/tmp/claude-gd-artefacts/venv/bin/python'
E0 = 0.7   # exposure of the scene-referred default
METHODS = {
    'none': ['5', '--off'], 'clip': ['0'], 'lch': ['1'], 'inpaint': ['2'], 'laplacian': ['3'],
    'segments': ['4'], 'opposed': ['5'], 'gd': ['6', '--full'], 'gd-x1': ['6', '--full', '--exp', '1'],
    'gd-x2': ['6', '--full', '--exp', '2'], 'gd-wb4': ['6', '--full', '--wb', '4'],
    'gd-wb4x2': ['6', '--full', '--wb', '4', '--exp', '2'],
}


def render(raw, out, args, ev, synth, size):
    xmp = out[:-4] + '.xmp'
    subprocess.run([PY, SIDE, xmp] + args + ['--ev', str(ev), '--black', '0'], check=True)
    env = dict(os.environ)
    env.pop('HL_SYNTH_EV', None)
    if synth is not None:
        env['HL_SYNTH_EV'] = '%g' % synth
    if os.path.exists(out):
        os.remove(out)
    # darktable-cli options go before --core, darktable options after it
    r = subprocess.run([DT, raw, xmp, out, '--width', str(size), '--height', str(size),
                        '--icc-type', 'LIN_REC2020', '--core', '--configdir', CFG, '--disable-opencl'],
                       env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not os.path.exists(out):
        raise RuntimeError('render failed: ' + r.stdout[-2000:])
    line = [l for l in r.stdout.splitlines() if 'TEMPDEBUG synthetic' in l]
    return read_pfm(out), (line[0].split('EV: ')[1] if line else None)


M = colour.RGB_COLOURSPACES['ITU-R BT.2020'].matrix_RGB_to_XYZ
TO_SRGB = colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['ITU-R BT.2020'], colour.RGB_COLOURSPACES['sRGB'])


def save_png(rgb, path):
    from PIL import Image
    a = np.clip(rgb @ TO_SRGB.T, 0, 1)
    s = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    Image.fromarray((s * 255 + 0.5).astype(np.uint8)).save(path)


def done_with(path, rgb):
    save_png(rgb, path[:-4] + '.png')
    if not os.environ.get('SYNTH_KEEP_PFM'):
        os.remove(path)


def lab(rgb):
    return colour.XYZ_to_Lab(np.maximum(rgb, 0) @ M.T)


def main():
    raw, k, wd = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    methods = sys.argv[4:] or list(METHODS)
    if 'none' not in methods:
        methods = ['none'] + methods
    size = int(os.environ.get('SYNTH_SIZE', '1600'))
    os.makedirs(wd, exist_ok=True)
    truth, _ = render(raw, os.path.join(wd, 'truth.pfm'), METHODS['none'], E0, None, size)
    truth_opp, _ = render(raw, os.path.join(wd, 'truth-opposed.pfm'), METHODS['opposed'], E0, None, size)
    done_with(os.path.join(wd, 'truth.pfm'), truth)
    done_with(os.path.join(wd, 'truth-opposed.pfm'), truth_opp)
    Lt = lab(truth)
    real = colour.delta_E(Lt, lab(truth_opp), method='CIE 2000') > 0.5
    res = dict(raw=raw, k=k, size=list(truth.shape[:2]), real_clipped=float(real.mean()), methods={})
    affected = None
    for m in methods:
        img, clipinfo = render(raw, os.path.join(wd, m + '.pfm'), METHODS[m], E0 - k, k, size)
        dE = colour.delta_E(Lt, lab(img), method='CIE 2000')
        done_with(os.path.join(wd, m + '.pfm'), img)
        if m == 'none':
            affected = (dE > 2) & ~real
            Lm = lab(img)
            Yt, Yn = (truth @ M.T)[..., 1], np.maximum((img @ M.T)[..., 1], 1e-6)
            deep = affected & (Yt / Yn > 2)
            chroma = np.hypot(Lt[..., 1], Lt[..., 2]) / np.maximum(Lt[..., 0], 1)
            colored = affected & (chroma > 0.25)
            res.update(clipped_photosites=clipinfo, affected=float(affected.mean()),
                       deep=float(deep.mean()), colored=float(colored.mean()))
            np.savez_compressed(os.path.join(wd, 'masks.npz'), affected=affected, deep=deep, colored=colored, real=real)
        sel = {'all': affected, 'deep': deep, 'colored': colored, 'deep_colored': deep & colored}
        st = {}
        for name, msk in sel.items():
            if msk.sum() < 50:
                continue
            v = dE[msk]
            # lightness and chroma errors separately: the brightness of a fully clipped
            # area is unknowable, its color is the question
            Li = lab(img[msk]); Lr = Lt[msk]
            dC = np.hypot(Li[:, 1] - Lr[:, 1], Li[:, 2] - Lr[:, 2])
            st[name] = dict(n=int(msk.sum()), dE_mean=float(v.mean()), dE_p50=float(np.median(v)),
                            dE_p90=float(np.percentile(v, 90)), dL_mean=float((Li[:, 0] - Lr[:, 0]).mean()),
                            dab_mean=float(dC.mean()))
        res['methods'][m] = st
        print('%-10s %s' % (m, '  '.join('%s dE %.1f/%.1f dab %.1f' % (n, s['dE_mean'], s['dE_p90'], s['dab_mean']) for n, s in st.items())), flush=True)
    json.dump(res, open(os.path.join(wd, 'scores.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
