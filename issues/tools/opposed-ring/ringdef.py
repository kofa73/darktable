# ringdef.py: issue 29 option (c). Inpaint opposed's brightness deficit in partly clipped pixels,
# split by how many channels are clipped (F >= 0.5 per channel, masks.npz) and by the distance to
# the nearest fully clipped pixel. log2(Y_truth / Y_opposed), Y = Rec2020 luminance, pixels with a
# truth only (deficit2.py's noTruth). Pooled over all 103 pairs (pixel weighted) and per scene kind.
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2
R = '/tmp/hl-rules-study/runs/'
pairs = [l.strip() for l in open('/tmp/hl-rules-study/scratch/included.txt') if l.strip()]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)
W = np.array([0.2627, 0.6780, 0.0593])
M = os.environ.get('RD_METHOD', 'opposed')
acc = {}
def add(key, v):
    acc.setdefault(key, []).append(v)
for p in pairs:
    wd = R + p; al = wd + '/align'
    sc = json.load(open(wd + '/scores.json')); s = 2.0 ** (sc['k'] - ev2.E0)
    kind = sc['ref'].split('/bracketed/')[1].split('/')[0]
    ta = cv2.imread(al + '/al_0001.tif', cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]), cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5: Ra = Ra / np.iinfo(mk.dtype).max
    noT = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk(5)) > 0
    F = np.load(wd + '/masks.npz')['F'].astype(np.float32)
    full = F.min(2) >= 0.5
    nclip = (F >= 0.5).sum(2)
    part = (F.max(2) > 0) & ~full
    dist = cv2.distanceTransform((~full).astype(np.uint8), cv2.DIST_L2, 5) if full.any() else np.full(full.shape, 1e9, np.float32)
    yt = np.maximum(T @ W, 1e-6)
    y = np.maximum((ev2.read_pfm(wd + '/' + M + '.pfm').astype(np.float32) * s) @ W, 1e-6)
    d = np.log2(yt / y)
    for nc in (0, 1, 2):
        for dn, sel_d in (('near', dist <= 4), ('mid', (dist > 4) & (dist <= 16)), ('far', dist > 16)):
            sel = part & ~noT & (nclip == nc) & sel_d
            if sel.sum() > 50:
                add(('all', nc, dn), d[sel]); add((kind, nc, dn), d[sel])
print('%s: log2(truth / method) in partly clipped pixels; nclip = channels with >= half of their photosites clipped' % M)
print('%-9s %5s %5s %10s %7s %7s %7s %7s' % ('kind', 'nclip', 'dist', 'pixels', 'median', 'p25', 'p75', 'p90'))
for k in sorted(acc, key=lambda k: (k[0] != 'all', k)):
    v = np.concatenate(acc[k])
    print('%-9s %5d %5s %10d %7.2f %7.2f %7.2f %7.2f' % (k[0], k[1], k[2], v.size, np.median(v), *np.percentile(v, [25, 75, 90])))
