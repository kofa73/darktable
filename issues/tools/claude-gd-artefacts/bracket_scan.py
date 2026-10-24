# bracket_scan.py <groups.json> <image dir> <out.json>
# For each bracket group, compares its darkest and brightest raw frames to see
# whether the pair can serve as ground truth for highlight reconstruction: the
# bright frame should clip where the dark frame does not.
#
# Works on 2x2 Bayer quads (R, mean G, B), normalized to the white level that
# darktable's rawspeed uses for the camera, so "clipped" means what highlights
# would see. The real exposure ratio is measured, not taken from EXIF: the
# LX7 brackets mix aperture changes, and nominal f-numbers are not exact. It is
# the median ratio of 16x16-quad block means where the bright frame is not
# clipped, which tolerates the small misalignment of handheld brackets.
import sys, json, os
import numpy as np, rawpy
import multiprocessing as mp

# rawspeed white levels (src/external/rawspeed/data/cameras.xml); rawpy's are
# the container maximum, which the LX7 never reaches after its own clipping
WHITE = {'DMC-LX7': 3971, 'DMC-TZ101': 4095, 'NIKON D40': 3880, 'NIKON D90': 3767,
         # CR2 SpecularWhiteLevel (Cr2Decoder.cpp:408); the sensor saturates at about 15283
         'Canon EOS 5D Mark III': 15094, 'NIKON D2X': 3880}


def white_level(camera, r):
    if camera.startswith(('NIKON D7000', 'NIKON D7100')):
        return 15892 if r.white_level > 4095 else 3972
    return WHITE.get(camera.split(' #')[0], r.white_level)


def quads(path, camera):
    with rawpy.imread(path) as r:
        a = r.raw_image_visible.astype(np.float32)
        cols = r.raw_colors_visible
        black = np.array(r.black_level_per_channel, np.float32)
        white = white_level(camera, r)
        wb = np.array(r.camera_whitebalance[:3], np.float64)
        a = (a - black[cols]) / (white - black[cols])
        h, w = (a.shape[0] // 2) * 2, (a.shape[1] // 2) * 2
        a, cols = a[:h, :w], cols[:h, :w]
        q = np.zeros((h // 2, w // 2, 3), np.float32)
        clip = np.zeros((h // 2, w // 2, 3), bool)
        for dy in (0, 1):
            for dx in (0, 1):
                c = int(cols[dy, dx])
                ch = 1 if c == 3 else c   # second green
                v = a[dy::2, dx::2]
                q[..., ch] += v * (0.5 if ch == 1 else 1.0)
                clip[..., ch] |= v >= 1.0
    return q, clip, wb / wb[1]


def blockmean(x, s=16):
    h, w = (x.shape[0] // s) * s, (x.shape[1] // s) * s
    return x[:h, :w].reshape(h // s, s, w // s, s, *x.shape[2:]).mean((1, 3))


def analyze(g):
    # merged DNGs and other files that were not copied are skipped
    fr = [f for f in g['frames'] if f['ev'] is not None and os.path.exists(os.path.join(analyze.src, f['file']))]
    if len(fr) < 2:
        return None
    fr.sort(key=lambda f: f['ev'])
    D, B = fr[0], fr[-1]
    src = analyze.src
    try:
        qd, cd, wb = quads(os.path.join(src, D['file']), g['camera'])
        qb, cb, _ = quads(os.path.join(src, B['file']), g['camera'])
    except Exception as e:
        return dict(time=g['time'], error=str(e))
    if qd.shape != qb.shape:
        return dict(time=g['time'], error='size mismatch')
    n = qd.shape[0] * qd.shape[1]
    # measured exposure ratio from green block means where the bright frame is unclipped
    gd, gb = blockmean(qd[..., 1]), blockmean(qb[..., 1])
    okb = blockmean(cb.any(2).astype(np.float32)) == 0
    # low limits, so that night scenes with few bright blocks still get a measurement
    sel = okb & (gd > 0.003) & (gb > 0.02)
    ev_meas = float(np.median(np.log2(gb[sel] / gd[sel]))) if sel.sum() > 20 else None
    res = dict(time=g['time'], camera=g['camera'], dark=D['file'], bright=B['file'],
               ev_nominal=B['ev'] - D['ev'], ev_measured=ev_meas,
               dark_clipped=float(cd.any(2).mean()), bright_clipped=float(cb.any(2).mean()),
               bright_all3=float(cb.all(2).mean()))
    # night scenes have a dark mean; sharpness is the 99th percentile of the
    # log-green gradient, which drops with motion blur (comparable only between
    # similar scenes, and between the two frames of a pair)
    def sharp(q):
        lg = np.log(np.maximum(q[..., 1], 0) + 0.002)
        return float(np.percentile(np.hypot(np.diff(lg, axis=0)[:, :-1], np.diff(lg, axis=1)[:-1, :]), 99))
    res.update(dark_mean=float(qd[..., 1].mean()), bright_mean=float(qb[..., 1].mean()),
               sharp_dark=sharp(qd), sharp_bright=sharp(qb))
    if ev_meas is None:
        return res
    # where the dark frame, scaled to the bright exposure, would clip
    would = qd * 2 ** ev_meas >= 1.0
    nw = would.sum(2)
    anyw = nw > 0
    gt = anyw & ~cd.any(2)           # would clip, and the dark frame holds the truth
    res.update(would_clip=float(anyw.mean()), would_all3=float((nw == 3).mean()),
               truth_frac=float(gt.sum() / max(anyw.sum(), 1)))
    # color of the would-clip areas in the dark frame, white balanced as shot
    rgb = qd[gt] * wb[None, :].astype(np.float32)
    if len(rgb):
        mx = rgb.max(1)
        chroma = (mx - rgb.min(1)) / np.maximum(mx, 1e-6)
        res.update(colored=float((chroma > 0.3).mean()), colored_all3=float(((chroma > 0.3) & (nw[gt] == 3)).mean()),
                   chroma_p90=float(np.percentile(chroma, 90)))
    # agreement of the predicted and the actual clipping of the bright frame (alignment, exposure)
    res['clip_agreement'] = float((anyw & cb.any(2)).sum() / max((anyw | cb.any(2)).sum(), 1))
    return res


def init(src):
    analyze.src = src


if __name__ == '__main__':
    groups = json.load(open(sys.argv[1]))
    src, out = sys.argv[2], sys.argv[3]
    # rawpy's OpenMP can deadlock in forked workers
    mp.set_start_method('spawn')
    with mp.Pool(10, initializer=init, initargs=(src,)) as p:
        res = [r for r in p.map(analyze, groups, chunksize=4) if r]
    json.dump(res, open(out, 'w'), indent=1)
    print(len(res), 'groups analyzed,', sum('error' in r for r in res), 'errors')
