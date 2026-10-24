# ev2.py synth <raw> <k EV> <workdir> [methods...]
# ev2.py pair <reference raw> <clipped raw> <dEV> <workdir> [methods...]
# Second version of the highlight reconstruction scoring (synth_eval.py, pair_eval.py),
# after the review in /tmp/claude-gd-artefacts/codex/2026-10-11-20-08-codex-report.md.
#
# Rendering: as before (darktable-cli, no tone mapper, linear Rec2020 PFM, synthetic
# clipping through HL_SYNTH_EV), but with the exposure module's compensation flags
# off, so every frame renders at its raw brightness whatever its EXIF exposure bias.
#
# Scoring, per image:
# - all renders are scaled so that the clipping level (green, neutral) sits at Y = 1,
#   then tone mapped by rgb / (1 + Y), which keeps chromaticity, so the clipping
#   level lands at L* 76 and 8x above it at L* 95. dE is CIE 2000 on that.
# - duv is the CIE 1976 u'v' distance of the linear renders: tint, independent of
#   brightness. dlogY is log2 of the luminance ratio to the truth.
# - masks come from the raw clipping flags, not from the renders: per 2x2 quad the
#   fraction of clipped photosites of each channel, registered onto the render.
#   partial: some but not all channels clipped; full: all three; deep: clipped and
#   the truth at least 2x the render without reconstruction; colored: clipped and
#   truth C*/L* > 0.25 after tone mapping; rest: far from any clipping but above a
#   quarter of the clipping level, where a method should change nothing.
# - excluded: photosites already clipped in the truth frame, and for pairs pixels
#   near strong edges, where a residual shift between the frames shows as error.
# Writes scores.json, masks.npz and a tone-mapped sRGB PNG per render; the PFMs are
# deleted unless EV2_KEEP_PFM is set.
import sys, os, json, subprocess, time
import numpy as np, cv2, colour, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from pfm import read_pfm
from bracket_scan import white_level

DT = '/workspace/darktable/build/bin/darktable-cli'
CFG = os.environ.get('SYNTH_CFG', '/tmp/hlc-eval/cfg')
SIDE = '/tmp/claude-gd-artefacts/scripts/make_sidecar.py'
PY = '/tmp/claude-gd-artefacts/venv/bin/python'
EXIFTOOL = ['perl', '/tmp/exiftool-dl/Image-ExifTool-13.59/exiftool']
E0 = 0.7
METHODS = {
    'none': ['5', '--off'], 'clip': ['0'], 'lch': ['1'], 'inpaint': ['2'], 'laplacian': ['3'],
    'segments': ['4'], 'opposed': ['5'], 'gd': ['6', '--full'], 'gd-x1': ['6', '--full', '--exp', '1'],
    'gd-x2': ['6', '--full', '--exp', '2'], 'gd-wb4': ['6', '--full', '--wb', '4'],
    'gd-wb4x2': ['6', '--full', '--wb', '4', '--exp', '2'],
    # hue from the measured channels in pixels with one flagged channel (experiment 6),
    # and that with the hue fill of fully clipped pixels (7)
    'gd-x6': ['6', '--full', '--exp', '6'], 'gd-x7': ['6', '--full', '--exp', '7'],
    'gd-wb4x6': ['6', '--full', '--wb', '4', '--exp', '6'], 'gd-wb4x7': ['6', '--full', '--wb', '4', '--exp', '7'],
    # experiment 6 with white instead of the hue estimate in fully clipped pixels (8)
    'gd-x8': ['6', '--full', '--exp', '8'], 'gd-wb4x8': ['6', '--full', '--wb', '4', '--exp', '8'],
    # opposed's estimate as the hue of partly clipped pixels, hue fill in fully clipped (9)
    'gd-x9': ['6', '--full', '--exp', '9'], 'gd-wb4x9': ['6', '--full', '--wb', '4', '--exp', '9'],
    # bright border: experiments 7 and 8 with the hue taken only from border pixels at least
    # half as bright as the clipping level (10, 11)
    'gd-wb4x10': ['6', '--full', '--wb', '4', '--exp', '10'], 'gd-wb4x11': ['6', '--full', '--wb', '4', '--exp', '11'],
    # opposed's estimate as the hue of partly clipped pixels, white in fully clipped (12)
    'gd-wb4x12': ['6', '--full', '--wb', '4', '--exp', '12'],
    # edge hue to white in fully clipped, with the hue of 7, 10 and 9 in partly clipped (13, 14, 15)
    'gd-wb4x13': ['6', '--full', '--wb', '4', '--exp', '13'], 'gd-wb4x14': ['6', '--full', '--wb', '4', '--exp', '14'],
    'gd-wb4x15': ['6', '--full', '--wb', '4', '--exp', '15'],
}
# methods with extra environment for darktable, for experiments behind TEMPDEBUG switches
METHOD_ENV = {}
for spec in os.environ.get('EV2_EXTRA', '').split(';'):
    # name=mode,args...|VAR=value,VAR=value
    if spec:
        name, rest = spec.split('=', 1)
        args, _, env = rest.partition('|')
        METHODS[name] = args.split(',')
        METHOD_ENV[name] = dict(e.split('=', 1) for e in env.split(',') if e)

M = colour.RGB_COLOURSPACES['ITU-R BT.2020'].matrix_RGB_to_XYZ
TO_SRGB = colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['ITU-R BT.2020'], colour.RGB_COLOURSPACES['sRGB'])


TIMES = {}


def render(raw, out, method, ev, synth, size, tone=None):
    xmp = out[:-4] + '.xmp'
    subprocess.run([PY, SIDE, xmp] + METHODS[method] + ['--ev', str(ev), '--black', '0', '--no-comp']
                   + (['--tone', tone] if tone else []), check=True)
    t0 = time.time()
    env = dict(os.environ)
    env.pop('HL_SYNTH_EV', None)
    env.update(METHOD_ENV.get(method, {}))
    if synth is not None:
        env['HL_SYNTH_EV'] = '%g' % synth
    if os.path.exists(out):
        os.remove(out)
    # darktable-cli options go before --core, darktable options after it
    r = subprocess.run([DT, raw, xmp, out, '--width', str(size), '--height', str(size),
                        '--icc-type', 'LIN_REC2020', '--core', '--configdir', CFG, '--disable-opencl']
                       # parallel workers each running 12 OpenMP threads on 12 cores slow down 5-10x
                       + (['--threads', os.environ['EV2_THREADS']] if os.environ.get('EV2_THREADS') else []),
                       env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    TIMES[os.path.basename(out)] = round(time.time() - t0, 2)
    if not os.path.exists(out):
        raise RuntimeError('render failed: ' + r.stdout[-2000:])
    img = read_pfm(out).astype(np.float32)
    if not os.environ.get('EV2_KEEP_PFM'):
        os.remove(out)
    return img


def lum(rgb):
    return rgb @ M[1]


def tone(rgb, s):
    # negative components (out of gamut after the camera matrix) are clamped first, as
    # in the review, so that chromaticity is defined
    a = np.maximum(rgb, 0) * s
    return a / (1 + lum(a))[..., None]


def lab(rgb_toned):
    return colour.XYZ_to_Lab(rgb_toned @ M.T)


def uv(rgb):
    X = np.maximum(rgb, 0) @ M.T
    d = np.maximum(X[..., 0] + 15 * X[..., 1] + 3 * X[..., 2], 1e-12)
    return np.stack([4 * X[..., 0] / d, 9 * X[..., 1] / d], -1)


def save_png(rgb_toned, path):
    from PIL import Image
    a = np.clip(rgb_toned @ TO_SRGB.T, 0, 1)
    s = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    Image.fromarray((s * 255 + 0.5).astype(np.uint8)).save(path)


def model(path):
    return subprocess.run(EXIFTOOL + ['-s3', '-Model', path], capture_output=True, text=True).stdout.strip()


def raw_flags(path, gain):
    """Per 2x2 quad: fraction of clipped photosites of R, G, B after scaling the
    normalized raw data by gain, and the quad's mean green, for registration."""
    cam = model(path)
    with rawpy.imread(path) as r:
        a = r.raw_image_visible.astype(np.float32)
        cols = r.raw_colors_visible
        black = np.array(r.black_level_per_channel, np.float32)
        white = white_level(cam, r)
        a = (a - black[cols]) / (white - black[cols]) * gain
        h, w = (a.shape[0] // 2) * 2, (a.shape[1] // 2) * 2
        a, cols = a[:h, :w], cols[:h, :w]
        f = np.zeros((h // 2, w // 2, 3), np.float32)
        g = np.zeros((h // 2, w // 2), np.float32)
        for dy in (0, 1):
            for dx in (0, 1):
                c = int(cols[dy, dx])
                ch = 1 if c == 3 else c
                v = a[dy::2, dx::2]
                # flagged as the methods flag: gradient domain from 0.98 of the clipping
                # level, opposed from 0.987 (highlights.c:56). Some cameras saturate a
                # channel just below the white level: the D2X's second green at 3871 of 3880
                f[..., ch] += (v >= 0.98) * (0.5 if ch == 1 else 1.0)
                if ch == 1:
                    g += 0.5 * v / gain
    return f, g, cam


def _dihedral(a, t):
    if t & 4:
        a = np.swapaxes(a, 0, 1)
    if t & 1:
        a = a[::-1]
    if t & 2:
        a = a[:, ::-1]
    return np.ascontiguousarray(a)


def register(g, Y):
    """Finds the flip, scale and shift that map quad image g onto the render's
    luminance Y: darktable applies the EXIF orientation and crops slightly
    differently from LibRaw. Returns a function that maps a quad map onto Y."""
    H, W = Y.shape
    # the floor keeps dark noise, which differs between LibRaw and darktable, from
    # dominating the log images
    norm = lambda a: np.log(np.maximum(a / np.median(a[a > 0]), 0.05)).astype(np.float32)
    ly = norm(Y)
    ly = (ly - ly.mean()) / (ly.std() + 1e-9)
    best = None
    for t in range(8):
        s = _dihedral(g, t)
        if abs(s.shape[1] / s.shape[0] - W / H) > 0.03:
            continue
        r = cv2.resize(s, (W, H), interpolation=cv2.INTER_AREA)
        lr = norm(r)
        lr = (lr - lr.mean()) / (lr.std() + 1e-9)
        win = cv2.createHanningWindow((W, H), cv2.CV_32F)
        # OpenCV 5.0's phaseCorrelate applies the window to its inputs in place
        (sx, sy), _ = cv2.phaseCorrelate(lr.copy(), ly.copy(), win)
        A = np.float32([[1, 0, sx], [0, 1, sy]])
        lr2 = cv2.warpAffine(lr, A, (W, H), borderMode=cv2.BORDER_REPLICATE)
        c = float((lr2 * ly).mean())
        if best is None or c > best[0]:
            best = (c, t, A)
    c, t, A = best

    def apply(m):
        r = cv2.resize(_dihedral(m, t), (W, H), interpolation=cv2.INTER_AREA)
        return cv2.warpAffine(r, A, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    return apply, dict(correlation=c, transform=t, shift=[float(A[0, 2]), float(A[1, 2])])


def stats(t_lab, t_uv, t_Y, img, s, msk):
    Li = lab(tone(img[msk], s))
    Lr = t_lab[msk]
    dE = colour.delta_E(Lr, Li, method='CIE 2000')
    duv = np.linalg.norm(uv(img[msk]) - t_uv[msk], axis=-1)
    # the same clamp of negative components as for the truth's luminance
    Yi = np.maximum(lum(np.maximum(img[msk], 0)), 1e-9)
    dl = np.log2(Yi / np.maximum(t_Y[msk], 1e-9))
    return dict(n=int(msk.sum()), dE_mean=float(dE.mean()), dE_p50=float(np.median(dE)),
                dE_p90=float(np.percentile(dE, 90)), duv_mean=float(duv.mean()),
                duv_p90=float(np.percentile(duv, 90)), dlogY_abs=float(np.abs(dl).mean()),
                dlogY=float(dl.mean()),
                dC_mean=float(np.hypot(Li[:, 1] - Lr[:, 1], Li[:, 2] - Lr[:, 2]).mean()))


SRGB_TO_XYZ = colour.RGB_COLOURSPACES['sRGB'].matrix_RGB_to_XYZ


def lab_display(rgb2020):
    # what an sRGB display shows: the display-referred Rec2020 output converted to
    # sRGB and clipped to its gamut, before any 8-bit quantization
    a = np.clip(rgb2020 @ TO_SRGB.T, 0, 1)
    return colour.XYZ_to_Lab(a @ SRGB_TO_XYZ.T)


def structure(t_L, i_L, msk):
    """Lightness detail inside msk: correlation of the L* gradients with the truth's
    (0 when either is flat), and the ratio of local contrast, the std of L* minus its
    blur of sigma 3, to the truth's (below 1: flatter than the truth)."""
    gx = lambda a: cv2.Sobel(a, cv2.CV_32F, 1, 0)
    gy = lambda a: cv2.Sobel(a, cv2.CV_32F, 0, 1)
    hp = lambda a: a - cv2.GaussianBlur(a, (0, 0), 3)
    v1 = np.concatenate([gx(t_L)[msk], gy(t_L)[msk]]); v2 = np.concatenate([gx(i_L)[msk], gy(i_L)[msk]])
    gc = float(np.corrcoef(v1, v2)[0, 1]) if v1.std() > 1e-6 and v2.std() > 1e-6 else 0.0
    return gc, float(hp(i_L)[msk].std() / max(hp(t_L)[msk].std(), 1e-6))


def patches(dE, region, area_min=25):
    """Connected areas of dE > 5 in region (the clipped pixels and a band of 3 px
    around them): their number from area_min pixels, the largest and the total area,
    both relative to the region."""
    n, lab_, st, _ = cv2.connectedComponentsWithStats(((dE > 5) & region).astype(np.uint8), connectivity=8)
    areas = st[1:, cv2.CC_STAT_AREA]
    big = areas[areas >= area_min]
    r = max(int(region.sum()), 1)
    return dict(count=int(len(big)), largest=float(big.max() / r) if len(big) else 0.0, total=float(big.sum() / r))


def display(res, wd, methods, ref, clp, ev_m, synth, size, A, t_Y, masks, near=None, excl=None, s=None, none=None):
    """EV2_DISPLAY=smooth: scores what a user sees. Exposure is set for the mid-tones,
    the median luminance of the truth at 0.18, then sigmoid's "smooth" preset maps the
    scene to the display. dE2000 on the display-referred output (white at Y = 1); the
    masks are those of the linear renders."""
    tone = os.environ['EV2_DISPLAY']
    rule = os.environ.get('EV2_EXPOSURE', 'median')
    if rule == 'default':
        # darktable's scene-referred default exposure on the clipped frame
        ec = E0
    elif rule == 'clipmid':
        # as trim, but on the clipped frame, which is several EV brighter than the
        # reference and so far less noisy in the shadows
        dark = float(os.environ.get('EV2_DARK_EV', '7'))
        n_Y = lum(np.maximum(none, 0))
        mid = ~near & ~excl & (n_Y * s > 2.0 ** -dark)
        res['display_mid_fraction'] = float(mid.mean())
        ec = E0 if mid.mean() < 0.05 else ev_m + np.log2(0.18 / max(float(np.median(n_Y[mid])), 1e-6))
        ec = float(np.clip(ec, 2.0 + np.log2(0.18), 5.0 + np.log2(0.18)))
    elif rule == 'trim':
        # the median at 0.18 of the mid-tones only: without the surroundings of the
        # clipped areas (light sources, speculars, reflections) and without the deep
        # shadows, more than EV2_DARK_EV below the clipping level
        dark = float(os.environ.get('EV2_DARK_EV', '7'))
        mid = ~near & ~excl & (t_Y * s > 2.0 ** -dark)
        res['display_mid_fraction'] = float(mid.mean())
        if mid.mean() < 0.05:
            ec = E0
        else:
            ec = ev_m + np.log2(0.18 / max(float(np.median(t_Y[mid])), 1e-6))
        ec = float(np.clip(ec, 2.0 + np.log2(0.18), 5.0 + np.log2(0.18)))
    else:
        # median luminance of the truth at 0.18, but the clipping level of the clipped
        # frame (raw 1.0, at 2^ec after exposure ec) kept 2 to 5 EV above 0.18: a dark
        # night scene would otherwise be lifted until sigmoid renders every highlight white
        ec = ev_m + np.log2(0.18 / max(float(np.median(t_Y)), 1e-6))
        ec = float(np.clip(ec, 2.0 + np.log2(0.18), 5.0 + np.log2(0.18)))
    # a shared offset for all methods, to see how the ranking depends on exposure
    ec += float(os.environ.get('EV2_EV_OFFSET', '0'))
    gain = ec - ev_m
    res.update(display=dict(tone=tone, rule=rule, exposure_clipped=ec, gain_ev=gain,
                            clip_above_grey_ev=ec - float(np.log2(0.18))))
    truth = render(ref, os.path.join(wd, 'truth_d.pfm'), 'none', E0 + gain, None, size, tone)
    if A is not None:
        truth = cv2.warpAffine(truth, A, truth.shape[1::-1], flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    t_lab = lab_display(truth)
    save_png(truth, os.path.join(wd, 'truth_d.png'))
    region = cv2.dilate(masks['all'].astype(np.uint8), np.ones((7, 7), np.uint8)) > 0
    if excl is not None:
        region &= ~excl
    for m in methods:
        img = render(clp, os.path.join(wd, m + '_d.pfm'), m, ev_m + gain, synth, size, tone)
        save_png(img, os.path.join(wd, m + '_d.png'))
        Li = lab_display(img)
        st = {}
        for n, v in masks.items():
            if v.sum() < 50:
                continue
            dE = colour.delta_E(t_lab[v], Li[v], method='CIE 2000')
            st[n] = dict(n=int(v.sum()), dE_mean=float(dE.mean()), dE_p50=float(np.median(dE)),
                         dE_p90=float(np.percentile(dE, 90)), gt5=float((dE > 5).mean()),
                         gt10=float((dE > 10).mean()))
            if n in ('all', 'partial', 'full', 'deep'):
                st[n]['gcorr'], st[n]['contrast'] = structure(t_lab[..., 0].astype(np.float32),
                                                              Li[..., 0].astype(np.float32), v)
        dEmap = colour.delta_E(t_lab, Li, method='CIE 2000')
        st['patches'] = patches(dEmap, region)
        res['methods'][m] = st
        res['times'] = TIMES
        print('%-10s %s' % (m, '  '.join('%s %.1f/%.0f%%' % (n, x['dE_mean'], 100 * x['gt5'])
                                         for n, x in st.items() if n != 'patches')), flush=True)
        json.dump(res, open(os.path.join(wd, 'scores.json.part'), 'w'), indent=1)
    os.replace(os.path.join(wd, 'scores.json.part'), os.path.join(wd, 'scores.json'))


def main():
    kind = sys.argv[1]
    if kind == 'synth':
        ref = clp = sys.argv[2]
        k, wd, methods = float(sys.argv[3]), sys.argv[4], sys.argv[5:]
        synth = k
    elif kind == 'pair':
        ref, clp = sys.argv[2], sys.argv[3]
        k, wd, methods = float(sys.argv[4]), sys.argv[5], sys.argv[6:]
        synth = None
    else:
        raise SystemExit('synth or pair')
    methods = methods or ['none', 'opposed', 'segments', 'inpaint', 'lch', 'gd', 'gd-x1', 'gd-x2', 'gd-wb4', 'gd-wb4x2']
    if 'none' not in methods:
        methods = ['none'] + methods
    size = int(os.environ.get('SYNTH_SIZE', '1600'))
    os.makedirs(wd, exist_ok=True)
    # the clipping level of the clipped renders, raw 1.0 at exposure E0 - k, goes to Y = 1
    s = 2.0 ** (k - E0)
    ev_m = E0 - k

    truth = render(ref, os.path.join(wd, 'truth.pfm'), 'none', E0, None, size)
    none = render(clp, os.path.join(wd, 'none.pfm'), 'none', ev_m, synth, size)
    if truth.shape != none.shape:
        raise SystemExit('size mismatch %s %s' % (truth.shape, none.shape))
    res = dict(kind=kind, ref=ref, clipped=clp, k=k, methods={})
    logY = lambda a: np.log(np.maximum(lum(np.maximum(a, 0)), 1e-5)).astype(np.float32)
    A = None
    if kind == 'pair':
        # tripod frames can still be shifted slightly: align the truth to the clipped frame
        win = cv2.createHanningWindow(truth.shape[1::-1], cv2.CV_32F)
        (sx, sy), resp = cv2.phaseCorrelate(logY(none), logY(truth), win)
        A = np.float32([[1, 0, -sx], [0, 1, -sy]])
        truth = cv2.warpAffine(truth, A, truth.shape[1::-1], flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        res['pair_shift'] = [float(sx), float(sy), float(resp)]
    t_Y = lum(np.maximum(truth, 0))
    t_tone = tone(truth, s)
    t_lab, t_uv = lab(t_tone), uv(truth)
    save_png(t_tone, os.path.join(wd, 'truth.png'))

    # raw flags of the clipped frame (synthetic: the truth frame raised by k), and of the
    # truth frame's own clipping, which leaves no ground truth. Registered on the render
    # of the clipped frame: tripod pairs share the raw geometry up to the shift above.
    fc, g, cam = raw_flags(clp, 2.0 ** k if synth is not None else 1.0)
    fr = raw_flags(ref, 1.0)[0]
    apply, reg = register(g, t_Y if kind == 'synth' else lum(np.maximum(none, 0)))
    F = np.stack([apply(fc[..., c]) for c in range(3)], -1)
    R = apply(fr.max(2))
    if A is not None:
        # the truth frame's own clipping moves with the truth; pixels the shifted truth
        # does not cover, filled by the border mode, have no truth
        R = cv2.warpAffine(R, A, R.shape[::-1], flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=1)
        R = np.maximum(R, 1 - cv2.warpAffine(np.ones_like(R), A, R.shape[::-1], flags=cv2.INTER_LINEAR,
                                             borderMode=cv2.BORDER_CONSTANT, borderValue=0))
    res.update(camera=cam, registration=reg, clipped_photosites=float(fc.mean()),
               truth_clipped_photosites=float(fr.mean()))

    excl = cv2.dilate((R > 0.02).astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
    if kind == 'pair':
        # a residual shift shows as error along strong edges
        lt = np.log(np.maximum(t_Y, 1e-5)).astype(np.float32)
        gm = cv2.Sobel(lt, cv2.CV_32F, 1, 0) ** 2 + cv2.Sobel(lt, cv2.CV_32F, 0, 1) ** 2
        excl |= cv2.dilate((gm > np.percentile(gm, 90)).astype(np.uint8), np.ones((7, 7), np.uint8)) > 0
    # a quad with one of its two greens flagged counts as green clipped, as the
    # gradient domain grid flags a channel from any clipped photosite in its window
    anyc = F.max(2) >= 0.5
    full = F.min(2) >= 0.5
    near = cv2.dilate((F.max(2) > 0.02).astype(np.uint8), np.ones((11, 11), np.uint8)) > 0
    Yn = np.maximum(lum(np.maximum(none, 0)), 1e-9)
    deep = anyc & (t_Y / Yn > 2)
    C = np.hypot(t_lab[..., 1], t_lab[..., 2]) / np.maximum(t_lab[..., 0], 1)
    colored = anyc & (C > 0.25)
    ok = ~excl
    masks = {'all': anyc & ok, 'partial': anyc & ~full & ok, 'full': full & ok,
             'deep': deep & ok, 'colored': colored & ok, 'deep_colored': deep & colored & ok,
             # pairs: a reference 3-6 EV darker is mostly noise far below the clip level
             'rest': ~near & ok & (t_Y * s > 0.25)}
    res['masks'] = {n: float(v.mean()) for n, v in masks.items()}
    np.savez_compressed(os.path.join(wd, 'masks.npz'), F=F.astype(np.float16), R=R.astype(np.float16), **masks)

    if os.environ.get('EV2_DISPLAY'):
        display(res, wd, methods, ref, clp, ev_m, synth, size, A, t_Y, masks, near, excl, s, none)
        return

    for m in methods:
        img = none if m == 'none' else render(clp, os.path.join(wd, m + '.pfm'), m, ev_m, synth, size)
        save_png(tone(img, s), os.path.join(wd, m + '.png'))
        res['methods'][m] = st = {n: stats(t_lab, t_uv, t_Y, img, s, v) for n, v in masks.items() if v.sum() >= 50}
        print('%-10s %s' % (m, '  '.join('%s %.1f/%.1f' % (n, x['dE_mean'], 1000 * x['duv_mean'])
                                         for n, x in st.items())), flush=True)
        json.dump(res, open(os.path.join(wd, 'scores.json.part'), 'w'), indent=1)
    os.replace(os.path.join(wd, 'scores.json.part'), os.path.join(wd, 'scores.json'))


if __name__ == '__main__':
    main()
