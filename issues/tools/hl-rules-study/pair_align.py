# pair_align.py <ev2 workdir>...: real pairs, scored only where a truth exists.
# Needs the linear renders that ev2 keeps with EV2_KEEP_PFM (truth.pfm = unaligned render of the
# darker frame, <method>.pfm of the clipped frame) and its masks.npz.
# 1. Aligns the truth render to the clipped frame with Hugin's align_image_stack (linear float
#    TIFFs at the clipped frame's scale, s = 2^(k - E0); reference = the inpaint opposed render),
#    which models rotation and lens geometry, not only a shift.
# 2. Warps the truth frame's own clipping (raw flags, registered as ev2 does) with the same .pto
#    through nona, so that pixels the truth itself clips can be left out.
# 3. Scores only pixels within DIL px of the clipped frame's flagged pixels that are covered by the
#    aligned truth and are not within 5 px of its own clipping (DIL = 3 by default: the methods only
#    change clipped pixels; a wider band adds the truth frame's stronger noise, 2^k times the
#    clipped frame's after scaling, which SSIMULACRA 2 counts as structure; PA_HALF=1 scores at
#    half resolution): per method a composite (method
#    there, truth elsewhere) against the truth, linear output with the clipping level at display
#    white, at 0 and -1 EV (or the exposures in PA_EVS), SSIMULACRA 2.
# 4. Reports the share of the clipped pixels that have a truth, and residual motion: in a ring
#    5-25 px around the clipped pixels, the share of pixels whose luminance, blurred with a 2 px
#    Gaussian, differs between the aligned truth and the inpaint opposed render by more than 20%.
import sys, os, json, subprocess, shutil
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev2
from ssim_eval import score
from lin_eval import to_srgb8

DIL = int(os.environ.get('PA_DIL', '3'))
HALF = os.environ.get('PA_HALF') == '1'
# result file name, so that runs with other SSIM_METHODS do not overwrite each other
OUT = os.environ.get('PA_OUT', 'pair_align.json')
# exposures to score, in EV (issue score-display-clipping: at 0 and -1 EV a third to half of the truth
# in clipped areas is over display white, where each sRGB channel clips by itself)
EVS = [int(x) for x in os.environ.get('PA_EVS', '0,-1').split(',')]
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)


def wtif(path, rgb):
    cv2.imwrite(path, cv2.cvtColor(np.ascontiguousarray(rgb, np.float32), cv2.COLOR_RGB2BGR))


def main(wd):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    s = 2.0 ** (sc['k'] - ev2.E0)
    al = os.path.join(wd, 'align'); shutil.rmtree(al, ignore_errors=True); os.makedirs(al)
    truth = ev2.read_pfm(os.path.join(wd, 'truth.pfm')).astype(np.float32) * s
    imgs = {m: ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) * s for m in METHODS}
    # the truth's own clipping, in the truth render's geometry
    fr, gr, _ = ev2.raw_flags(sc['ref'], 1.0)
    apply, _ = ev2.register(gr, ev2.lum(np.maximum(truth, 0)))
    Rt = apply(fr.max(2)).astype(np.float32)
    wtif(os.path.join(al, 'ref.tif'), imgs['opposed']); wtif(os.path.join(al, 'tru.tif'), truth)
    wtif(os.path.join(al, 'mask.tif'), np.repeat(Rt[..., None], 3, 2))
    # strict control point error first; dark frames with few features need the default 3 px, and
    # some (t62 0048, g315) more control points per grid cell and 5 px
    for c, t in (('16', '1'), ('16', '3'), ('40', '5')):
        r = subprocess.run(['align_image_stack', '-l', '--align-to-first', '-c', c, '-g', '6', '-t', t,
                            '-p', 'pair.pto', '-a', 'al_', 'ref.tif', 'tru.tif'], cwd=al, capture_output=True, text=True)
        if not r.returncode:
            break
    if r.returncode:
        print('%-26s align_image_stack failed: %s' % (os.path.basename(wd.rstrip('/')), (r.stdout + r.stderr).strip().splitlines()[0]))
        return
    pto = open(os.path.join(al, 'pair.pto')).read()
    open(os.path.join(al, 'mask.pto'), 'w').write(pto.replace('tru.tif', 'mask.tif'))
    subprocess.run(['nona', '-m', 'TIFF_m', '-i', '1', '-o', 'mk_', 'mask.pto'], cwd=al, check=True, capture_output=True)
    # PA_ALIGN_ONLY=1: recreate align/ (aligned truth al_0001.tif, warped clipping mk_*.tif) and stop,
    # removing the alignment inputs; crop_sheet.py, region_oracle.py and the border scripts read only those two
    if os.environ.get('PA_ALIGN_ONLY') == '1':
        for f in ('ref.tif', 'tru.tif', 'mask.tif', 'al_0000.tif'):
            if os.path.exists(os.path.join(al, f)):
                os.remove(os.path.join(al, f))
        print('%-26s aligned' % os.path.basename(wd.rstrip('/')), flush=True)
        return
    ta = cv2.imread(os.path.join(al, 'al_0001.tif'), cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]),
                    cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5:   # integer output
        Ra = Ra / np.iinfo(mk.dtype).max
    noTruth = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk(5)) > 0
    M = np.load(os.path.join(wd, 'masks.npz'))
    clip = M['F'].astype(np.float32).max(2) > 0
    region = (cv2.dilate(clip.astype(np.uint8), disk(DIL)) > 0) & ~noTruth
    share = float((clip & ~noTruth).sum() / max(clip.sum(), 1))
    ring = (cv2.dilate(clip.astype(np.uint8), disk(25)) > 0) & ~(cv2.dilate(clip.astype(np.uint8), disk(5)) > 0) & ~noTruth
    # blurred: per-pixel noise of the darker frame (2^k times the clipped frame's after scaling)
    # exceeds 20% in dark areas and showed as motion (g319 26% unblurred, 2% blurred)
    yt, yo = (cv2.GaussianBlur(ev2.lum(np.maximum(x, 0)), (0, 0), 2) for x in (T, imgs['opposed']))
    ok = ring & (yt > 0.01)
    motion = float((np.abs(np.log(np.maximum(yo[ok], 1e-6) / np.maximum(yt[ok], 1e-6))) > np.log(1.2)).mean()) if ok.any() else None
    res = dict(truth_share=share, motion=motion, region_fraction=float(region.mean()))
    if os.environ.get('PA_MODE', 'blend') == 'blend':
        # Kofa: score the changed pixels and how they blend with their surroundings. Reference =
        # the clipped frame's own render (none.pfm, no reconstruction) wherever no method changed a
        # pixel (same scene, less noise than the darker frame), the aligned truth where any method
        # changed pixels (plus 2 px), and, per method, the method's own pixels where the truth is
        # clipped or missing, so those cannot count. Each method's full render is scored against
        # its reference: unchanged pixels are identical in both and only the changed areas and
        # their seams with the surroundings matter.
        N = ev2.read_pfm(os.path.join(wd, 'none.pfm')).astype(np.float32) * s
        rel = lambda x: np.abs(x - N).max(2) / np.maximum(N.max(2), 1e-3)
        changed = clip.copy()
        for img in imgs.values():
            changed |= rel(img) > 0.01
        changed = cv2.dilate(changed.astype(np.uint8), disk(2)) > 0
        res['changed_fraction'] = float(changed.mean())
        for off in EVS:
            g = 2.0 ** off
            for m, img in imgs.items():
                ref = N.copy(); ref[changed] = T[changed]; ref[noTruth] = img[noTruth]
                A = cv2.cvtColor(to_srgb8(ref * g), cv2.COLOR_RGB2BGR); B = cv2.cvtColor(to_srgb8(img * g), cv2.COLOR_RGB2BGR)
                cv2.imwrite(os.path.join(wd, 'bl%+d_ref_%s.png' % (off, m)), A)
                cv2.imwrite(os.path.join(wd, 'bl%+d_%s.png' % (off, m)), B)
                if HALF:
                    A = cv2.resize(A, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
                    B = cv2.resize(B, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
                res['%s %+d' % (m, off)] = score(A, B)
        json.dump(res, open(os.path.join(wd, OUT), 'w'), indent=1)
        print('%-26s truth for %3.0f%% of clipped px, motion %4.1f%%, changed %4.1f%%  ' % (
            os.path.basename(wd.rstrip('/')), 100 * share, 100 * (motion or 0), 100 * res['changed_fraction'])
            + '  '.join('%+dEV ' % off + ' '.join('%s %6.1f' % (m[:3], res['%s %+d' % (m, off)]) for m in METHODS)
                        for off in EVS), flush=True)
        return
    res = dict(truth_share=share, motion=motion, region_fraction=float(region.mean()))
    for off in EVS:
        g = 2.0 ** off
        t8 = to_srgb8(T * g)
        cv2.imwrite(os.path.join(wd, 'al%+d_truth.png' % off), cv2.cvtColor(t8, cv2.COLOR_RGB2BGR))
        for m, img in imgs.items():
            x8 = to_srgb8(img * g)
            comp = t8.copy(); comp[region] = x8[region]
            cv2.imwrite(os.path.join(wd, 'al%+d_%s.png' % (off, m)), cv2.cvtColor(comp, cv2.COLOR_RGB2BGR))
            A, B = cv2.cvtColor(t8, cv2.COLOR_RGB2BGR), cv2.cvtColor(comp, cv2.COLOR_RGB2BGR)
            if HALF:
                A = cv2.resize(A, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
                B = cv2.resize(B, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
            res['%s %+d' % (m, off)] = score(A, B)
    json.dump(res, open(os.path.join(wd, OUT), 'w'), indent=1)
    print('%-26s truth for %3.0f%% of clipped px, motion %4.1f%%  ' % (os.path.basename(wd.rstrip('/')), 100 * share,
                                                                      100 * (motion or 0))
          + '  '.join('%+dEV ' % off + ' '.join('%s %6.1f' % (m[:3], res['%s %+d' % (m, off)]) for m in METHODS) for off in EVS),
          flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)
