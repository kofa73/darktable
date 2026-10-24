# bracket_strips.py <groups.txt> <outdir> [workers]
# For a visual blur check: one strip per bracket of 1:1 crops (one quad per
# pixel, so half the sensor resolution) of all frames, taken from the block
# with the most detail that no frame clips, each frame aligned to the middle
# frame and scaled to its exposure. Motion blur and missed focus show directly,
# which a sharpness number cannot separate from noise and depth of field.
import sys, os, json, subprocess
import numpy as np
import multiprocessing as mp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bracket_scan2 import load, shift, parse_groups
from bracket_scan import blockmean

S = 160   # crop size in quads


def strip(arg):
    k, g, outdir = arg
    fr = sorted(g['frames'], key=lambda f: f['ev'])
    data = [load(f['path'], g['camera']) for f in fr]
    m = len(fr) // 2
    qm = data[m][0]
    # STRIP_CLIP_UPTO=n: only frames 0..n must be unclipped in the crop, for
    # night scenes where the bright frames clip every detailed area
    upto = int(os.environ.get('STRIP_CLIP_UPTO', len(fr) - 1))
    clip_any = np.zeros(qm.shape[:2], bool)
    for _, c, _ in data[:upto + 1]:
        clip_any |= c.any(2)
    gm = qm[..., 1]
    grad = np.zeros_like(gm)
    grad[:-1, :-1] = np.hypot(np.diff(gm, axis=0)[:, :-1], np.diff(gm, axis=1)[:-1, :])
    # detail relative to brightness, so a bright flat wall does not win
    score = blockmean(grad, S) / (blockmean(gm, S) + 0.01)
    bad = blockmean(clip_any.astype(np.float32), S) > 0
    dark = blockmean(data[0][0][..., 1], S) < 0.002
    score[bad | dark] = -1
    by, bx = np.unravel_index(np.argmax(score), score.shape)
    y0, x0 = by * S, bx * S
    tiles = []
    for f, (q, c, wb) in zip(fr, data):
        # align on a larger window around the crop
        pad = 64
        ya, yb = max(y0 - pad, 0), min(y0 + S + pad, gm.shape[0])
        xa, xb = max(x0 - pad, 0), min(x0 + S + pad, gm.shape[1])
        dy, dx, _ = shift(gm[ya:yb, xa:xb], q[ya:yb, xa:xb, 1])
        yy, xx = np.clip(y0 - dy, 0, gm.shape[0] - S), np.clip(x0 - dx, 0, gm.shape[1] - S)
        crop = q[yy:yy + S, xx:xx + S]
        ref = qm[y0:y0 + S, x0:x0 + S, 1]
        r = np.median(ref[ref > 0.01]) / max(np.median(crop[..., 1][ref > 0.01]), 1e-6) if (ref > 0.01).any() else 1
        rgb = crop * r * wb[None, None, :]
        rgb = rgb / max(np.percentile(qm[y0:y0 + S, x0:x0 + S] * wb, 99.5), 1e-6)
        img = (np.clip(rgb, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
        tiles.append(img)
    out = np.concatenate([np.pad(t, ((0, 0), (0, 4), (0, 0)), constant_values=255) for t in tiles], 1)
    path = os.path.join(outdir, 'b%02d.ppm' % k)
    with open(path, 'wb') as fh:
        fh.write(b'P6 %d %d 255\n' % (out.shape[1], out.shape[0]))
        fh.write(out.tobytes())
    label = 'b%02d  ' % k + '  '.join('%d:%s' % (i, f['shutter']) for i, f in enumerate(fr))
    subprocess.run(['convert', path, '-resize', '150%', '-gravity', 'north', '-splice', '0x18',
                    '-pointsize', '15', '-annotate', '+0+1', label, path[:-4] + '.png'], check=True)
    os.remove(path)
    return k


if __name__ == '__main__':
    gs = parse_groups(sys.argv[1])
    outdir = sys.argv[2]
    # STRIP_ONLY=3,17: only these groups, by index
    only = os.environ.get('STRIP_ONLY')
    sel = [int(v) for v in only.split(',')] if only else range(len(gs))
    os.makedirs(outdir, exist_ok=True)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    mp.set_start_method('spawn')
    with mp.Pool(n) as p:
        p.map(strip, [(k, gs[k], outdir) for k in sel], chunksize=1)
