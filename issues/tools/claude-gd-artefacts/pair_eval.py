# pair_eval.py <reference raw> <clipped raw> <dEV> <workdir> [methods...]
# Scores highlight reconstruction methods on a real pair: the clipped frame, rendered
# with each method and dEV lower exposure, against the reference frame rendered with
# highlights off. Renders as synth_eval.py, without artificial clipping. The
# reference is shifted onto the clipped frame by phase correlation of the unclipped
# areas; pixels within 3 px of strong edges are left out, since a residual shift
# shows there as error.
import sys, os, json, numpy as np, cv2, colour
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import synth_eval as se


def main():
    ref, clp, dev, wd = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
    methods = sys.argv[5:] or list(se.METHODS)
    if 'none' not in methods:
        methods = ['none'] + methods
    size = int(os.environ.get('SYNTH_SIZE', '1600'))
    os.makedirs(wd, exist_ok=True)
    truth, _ = se.render(ref, os.path.join(wd, 'truth.pfm'), se.METHODS['none'], se.E0, None, size)
    truth_opp, _ = se.render(ref, os.path.join(wd, 'truth-opposed.pfm'), se.METHODS['opposed'], se.E0, None, size)
    none, _ = se.render(clp, os.path.join(wd, 'none.pfm'), se.METHODS['none'], se.E0 - dev, None, size)
    if truth.shape != none.shape:
        raise SystemExit('size mismatch %s %s' % (truth.shape, none.shape))
    # register on log luminance where the clipped frame is not clipped
    Y = lambda a: np.log(np.maximum((a @ se.M.T)[..., 1], 1e-4)).astype(np.float32)
    yt, yn = Y(truth), Y(none)
    win = cv2.createHanningWindow(yt.shape[::-1], cv2.CV_32F)
    (sx, sy), resp = cv2.phaseCorrelate(yn, yt, win)
    A = np.float32([[1, 0, -sx], [0, 1, -sy]])
    shift = lambda a: cv2.warpAffine(a.astype(np.float32), A, a.shape[1::-1], flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    truth, truth_opp = shift(truth), shift(truth_opp)
    Lt = se.lab(truth)
    real = colour.delta_E(Lt, se.lab(truth_opp), method='CIE 2000') > 0.5
    g = cv2.Sobel(Y(truth), cv2.CV_32F, 1, 0) ** 2 + cv2.Sobel(Y(truth), cv2.CV_32F, 0, 1) ** 2
    edges = cv2.dilate((g > np.percentile(g, 90)).astype(np.uint8), np.ones((7, 7), np.uint8)) > 0
    res = dict(ref=ref, clipped=clp, dev=dev, shift=[float(sx), float(sy)], response=float(resp),
               real_clipped=float(real.mean()), methods={})
    se.done_with(os.path.join(wd, 'truth.pfm'), truth)
    se.done_with(os.path.join(wd, 'truth-opposed.pfm'), truth_opp)
    for m in methods:
        img = none if m == 'none' else se.render(clp, os.path.join(wd, m + '.pfm'), se.METHODS[m], se.E0 - dev, None, size)[0]
        dE = colour.delta_E(Lt, se.lab(img), method='CIE 2000')
        se.done_with(os.path.join(wd, m + '.pfm'), img)
        if m == 'none':
            affected = (dE > 2) & ~real & ~edges
            Yt, Yn = (truth @ se.M.T)[..., 1], np.maximum((img @ se.M.T)[..., 1], 1e-6)
            deep = affected & (Yt / Yn > 2)
            chroma = np.hypot(Lt[..., 1], Lt[..., 2]) / np.maximum(Lt[..., 0], 1)
            colored = affected & (chroma > 0.25)
            res.update(affected=float(affected.mean()), deep=float(deep.mean()), colored=float(colored.mean()))
            # the unclipped rest shows how well the two frames match at all
            rest = ~affected & ~real & ~edges & (Yt > 0.01)
            res['rest_dE_p50'] = float(np.median(dE[rest])) if m == 'none' else None
            np.savez_compressed(os.path.join(wd, 'masks.npz'), affected=affected, deep=deep, colored=colored, real=real, edges=edges)
        st = {}
        for name, msk in {'all': affected, 'deep': deep, 'colored': colored, 'deep_colored': deep & colored}.items():
            if msk.sum() < 50:
                continue
            v = dE[msk]; Li = se.lab(img[msk]); Lr = Lt[msk]
            st[name] = dict(n=int(msk.sum()), dE_mean=float(v.mean()), dE_p50=float(np.median(v)),
                            dE_p90=float(np.percentile(v, 90)), dL_mean=float((Li[:, 0] - Lr[:, 0]).mean()),
                            dab_mean=float(np.hypot(Li[:, 1] - Lr[:, 1], Li[:, 2] - Lr[:, 2]).mean()))
        res['methods'][m] = st
        print('%-10s %s' % (m, '  '.join('%s dE %.1f/%.1f dab %.1f' % (n, s['dE_mean'], s['dE_p90'], s['dab_mean']) for n, s in st.items())), flush=True)
    json.dump(res, open(os.path.join(wd, 'scores.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
