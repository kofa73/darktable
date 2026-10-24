# deficit.py: how much darker inpaint opposed (and gradient domain) leaves the clipped pixels than the
# truth. Per pair, pixels of the clipped frame's clipped area (masks.npz F > 0) that have a truth
# (pair_align.py's noTruth excluded), split into partly and fully clipped (F min >= 0.5): median of
# log2(Y_truth / Y_method), Y = Rec2020 luminance of the linear renders (no display clipping).
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts'); sys.path.insert(0, '/tmp/hl-rules-study/scripts')
import ev2
R = '/tmp/hl-rules-study/runs/'
pairs = [l.strip() for l in open('/tmp/hl-rules-study/scratch/included.txt') if l.strip()]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)
W = np.array([0.2627, 0.6780, 0.0593])
out = {}
for p in pairs:
    wd = R + p; al = wd + '/align'
    sc = json.load(open(wd + '/scores.json')); s = 2.0 ** (sc['k'] - ev2.E0)
    ta = cv2.imread(al + '/al_0001.tif', cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]), cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5: Ra = Ra / np.iinfo(mk.dtype).max
    noT = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk(5)) > 0
    F = np.load(wd + '/masks.npz')['F'].astype(np.float32)
    clip = F.max(2) > 0; full = F.min(2) >= 0.5
    yt = np.maximum(T @ W, 1e-6)
    r = {'scene': p.split('/')[-1].split('-')[0], 'kind': sc.get('kind')}
    for m in ('opposed', 'gd-r-mg4'):
        y = np.maximum((ev2.read_pfm(wd + '/' + m + '.pfm').astype(np.float32) * s) @ W, 1e-6)
        for name, sel in (('partial', clip & ~full & ~noT), ('full', full & ~noT)):
            r[m + ' ' + name] = (float(np.median(np.log2(yt[sel] / y[sel]))), int(sel.sum())) if sel.sum() > 50 else None
    out[p] = r
json.dump(out, open('/tmp/hl-rules-study/tasks/2026-10-24-brightness/deficit.json', 'w'), indent=1)
for m in ('opposed', 'gd-r-mg4'):
    for name in ('partial', 'full'):
        v = np.array([r[m + ' ' + name][0] for r in out.values() if r[m + ' ' + name]])
        print('%-9s %-8s pairs %3d: median deficit %.2f EV, quartiles %.2f / %.2f, pairs with deficit > 0.5 EV: %d'
              % (m, name, len(v), np.median(v), np.percentile(v, 25), np.percentile(v, 75), (v > 0.5).sum()))
