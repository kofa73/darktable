# bracket_scan2.py <groups.txt> <out.json> [workers]
# Rates every frame and every frame pair of handheld exposure brackets, as listed
# by find_brackets.py, for use as ground truth in highlight reconstruction:
#  - artificial clipping needs a darkest frame that clips (almost) nothing;
#  - a real pair needs a darker frame that holds the truth where a brighter one
#    clips, both sharp and aligned, with an exposure ratio that is the same
#    across the image (the TZ101 and LX7 change aperture inside a bracket, and
#    vignetting differs between f-numbers).
# Unlike bracket_scan.py it looks at all frames, measures the shift between
# them (handheld), and compares sharpness only on pixels well exposed in both
# frames, because the log gradient of the darkest frame is dominated by noise.
import sys, os, re, json
import numpy as np
import multiprocessing as mp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bracket_scan

# the TZ101 piles up at raw 4080-4097 with a white level of 4095, so a
# threshold at the white level misses saturated photosites; 0.99 of the
# range catches them, and matches the 0.98 flag of gradient domain closely
CLIP = 0.99


def load(path, camera):
    import rawpy
    with rawpy.imread(path) as r:
        a = r.raw_image_visible.astype(np.float32)
        cols = r.raw_colors_visible
        black = np.array(r.black_level_per_channel, np.float32)
        white = bracket_scan.white_level(camera, r)
        wb = np.array(r.camera_whitebalance[:3], np.float64)
        a = (a - black[cols]) / (white - black[cols])
        h, w = (a.shape[0] // 2) * 2, (a.shape[1] // 2) * 2
        q = np.zeros((h // 2, w // 2, 3), np.float32)
        clip = np.zeros((h // 2, w // 2, 3), bool)
        for dy in (0, 1):
            for dx in (0, 1):
                c = int(cols[dy, dx])
                ch = 1 if c == 3 else c
                v = a[dy:h:2, dx:w:2]
                q[..., ch] += v * (0.5 if ch == 1 else 1.0)
                clip[..., ch] |= v >= CLIP
    return q, clip, wb / wb[1]


def shift(a, b):
    # translation (dy, dx) in quads such that roll(b, dy, dx) matches a, by
    # phase correlation on log green at full quad resolution; also returns the
    # correlation peak, which drops with blur, rotation or scene motion
    def prep(x):
        x = np.log(np.maximum(x, 1e-4))
        x = x - x.mean()
        return x * np.outer(np.hanning(x.shape[0]), np.hanning(x.shape[1]))
    pa = prep(a)
    A, B = np.fft.rfft2(pa), np.fft.rfft2(prep(b))
    R = A * np.conj(B)
    R /= np.maximum(np.abs(R), 1e-12)
    r = np.fft.irfft2(R, s=pa.shape)
    i = np.unravel_index(np.argmax(r), r.shape)
    dy, dx = [(v if v < n // 2 else v - n) for v, n in zip(i, r.shape)]
    return int(dy), int(dx), float(r.max())


def roll(x, dy, dx):
    return np.roll(np.roll(x, dy, 0), dx, 1)


def sharp_ratio(gi, gj, mask):
    # 90th percentile of the green gradient magnitude in frame j relative to
    # frame i, on pixels where both are well exposed; below 1 means j is blurred
    def g(x):
        return np.hypot(np.diff(x, axis=0)[:, :-1], np.diff(x, axis=1)[:-1, :])
    m = mask[:-1, :-1]
    if m.sum() < 10000:
        return None
    a, b = g(gi)[m], g(gj)[m]
    return float(np.percentile(b, 90) / max(np.percentile(a, 90), 1e-9))


def analyze(g):
    try:
        fr = sorted(g['frames'], key=lambda f: f['ev'])
        data = [load(f['path'], g['camera']) for f in fr]
    except Exception as e:
        return dict(time=g['time'], error=str(e))
    wb = data[0][2]
    q0 = data[0][0]
    out = dict(time=g['time'], camera=g['camera'], frames=[])
    for f, (q, c, _) in zip(fr, data):
        out['frames'].append(dict(file=os.path.basename(f['path']), ev_nominal=f['ev'], shutter=f['shutter'],
                                  f=f['f'], clipped=float(c.any(2).mean()), all3=float(c.all(2).mean()),
                                  mean_g=float(q[..., 1].mean())))
    # shifts and measured exposure of every frame against the darkest one
    for k in range(1, len(fr)):
        dy, dx, peak = shift(q0[..., 1], data[k][0][..., 1])
        out['frames'][k].update(shift=[dy, dx], peak=peak)
    out['frames'][0].update(shift=[0, 0], peak=1.0)

    pairs = []
    for i in range(len(fr)):
        qi, ci, _ = data[i]
        for j in range(i + 1, len(fr)):
            qj, cj, _ = data[j]
            # bring frame i onto frame j's geometry; measured per pair, because
            # the darkest frame is too noisy to chain the shifts through it
            dy, dx, peak = shift(qj[..., 1], qi[..., 1])
            qi_s = roll(qi, dy, dx)
            ci_s = roll(ci, dy, dx)
            gi, gj = bracket_scan.blockmean(qi_s[..., 1]), bracket_scan.blockmean(qj[..., 1])
            okj = bracket_scan.blockmean(cj.any(2).astype(np.float32)) == 0
            sel = okj & (gi > 0.003) & (gj > 0.02)
            if sel.sum() < 50:
                continue
            lr = np.log2(gj[sel] / gi[sel])
            ev = float(np.median(lr))
            # ratio per ninth of the image: vignetting differs between apertures
            H, W = gi.shape
            yy, xx = np.nonzero(sel)
            reg = []
            for a in range(3):
                for b in range(3):
                    m = (yy * 3 // H == a) & (xx * 3 // W == b)
                    if m.sum() > 20:
                        reg.append(float(np.median(lr[m])))
            ev_spread = max(reg) - min(reg) if len(reg) > 1 else None
            clipj = cj.any(2)
            truth = qi_s * 2 ** ev
            pred = (truth >= CLIP).any(2)
            # sharpness on pixels well exposed in both, at frame j's exposure
            m = (truth[..., 1] > 0.05) & (truth[..., 1] < 0.8) & (qj[..., 1] > 0.05) & (qj[..., 1] < 0.8)
            sr = sharp_ratio(truth[..., 1], qj[..., 1], m)
            p = dict(i=i, j=j, shift=[dy, dx], peak=peak, ev=ev, ev_spread=ev_spread, same_f=fr[i]['f'] == fr[j]['f'],
                     j_clipped=float(clipj.mean()), j_all3=float(cj.all(2).mean()),
                     sharp_j_vs_i=sr)
            if clipj.sum():
                tmax = truth.max(2)[clipj]
                p.update(i_clipped_in_region=float(ci_s.any(2)[clipj].mean()),
                         deep=float((tmax > 2).mean()),
                         clip_agreement=float((pred & clipj).sum() / max((pred | clipj).sum(), 1)))
                ok = clipj & ~ci_s.any(2)
                rgb = qi_s[ok] * wb[None, :].astype(np.float32)
                if len(rgb):
                    mx = rgb.max(1)
                    chroma = (mx - rgb.min(1)) / np.maximum(mx, 1e-6)
                    p.update(colored=float((chroma > 0.3).mean()))
            pairs.append(p)
    out['pairs'] = pairs
    return out


def parse_groups(path):
    groups, cur = [], None
    for line in open(path):
        m = re.match(r'^(\S+ \S+)\s+(\S+(?: \S+)*?)\s+(\d+) frames', line)
        if m:
            cur = dict(time=m.group(1), camera=m.group(2), frames=[])
            groups.append(cur)
            continue
        m = re.match(r'^\s+([+-]?[\d.]+) EV\s+(\S+ s)\s+(f/\S+)\s+ISO \S+\s+(\S+)$', line)
        if m and cur is not None:
            cur['frames'].append(dict(ev=float(m.group(1)), shutter=m.group(2), f=m.group(3), path=m.group(4)))
    return groups


if __name__ == '__main__':
    groups = parse_groups(sys.argv[1])
    out = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    mp.set_start_method('spawn')
    with mp.Pool(n) as p:
        res = p.map(analyze, groups, chunksize=1)
    json.dump(res, open(out, 'w'), indent=1)
    print(len(res), 'groups analyzed,', sum('error' in r for r in res), 'errors')
