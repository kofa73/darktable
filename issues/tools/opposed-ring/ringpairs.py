# ringpairs.py: per pair, inpaint opposed's deficit log2(truth / opposed) in partly clipped pixels
# within 16 px of a fully clipped pixel, split by the number of clipped channels (1, 2), with the
# camera-space channel that clips (most clipped photosite share) for orientation
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2
R = '/tmp/hl-rules-study/runs/'
pairs = [l.strip() for l in open('/tmp/hl-rules-study/scratch/included.txt') if l.strip()]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)
W = np.array([0.2627, 0.6780, 0.0593])
rows = []
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
    if not full.any(): continue
    nclip = (F >= 0.5).sum(2)
    part = (F.max(2) > 0) & ~full
    dist = cv2.distanceTransform((~full).astype(np.uint8), cv2.DIST_L2, 5)
    yt = np.maximum(T @ W, 1e-6)
    y = np.maximum((ev2.read_pfm(wd + '/opposed.pfm').astype(np.float32) * s) @ W, 1e-6)
    d = np.log2(yt / y)
    r = [p.split('/')[-1][:38], kind]
    for nc in (1, 2):
        sel = part & ~noT & (nclip == nc) & (dist <= 16)
        if sel.sum() > 200:
            # which channels clip there (camera space, F per channel)
            ch = ''.join('rgb'[c] for c in range(3) if np.median(F[sel][:, c]) >= 0.5)
            r += ['%6.2f %7d %s' % (np.median(d[sel]), sel.sum(), ch)]
        else:
            r += ['%6s %7s %s' % ('-', '', '')]
    rows.append(r)
rows.sort(key=lambda r: -max(float(x.split()[0]) if x.split()[0] != '-' else -9 for x in r[2:]))
print('%-38s %-9s %-20s %-20s' % ('pair', 'kind', '1 clipped: dEV n ch', '2 clipped: dEV n ch'))
for r in rows: print('%-38s %-9s %-20s %-20s' % tuple(r))
