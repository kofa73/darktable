# ref_clip.py: share of the scored clipped-area pixels whose aligned truth (pair_align.py's reference)
# exceeds display white in sRGB, per channel, at 0 and -1 EV (lin_eval.to_srgb8 clips each channel
# at 1 after the Rec2020 -> sRGB matrix). "any" = at least one channel over 1 (hue distorted unless
# all are), "all" = all three over 1 (plain white).
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts'); sys.path.insert(0, '/tmp/hl-rules-study/scripts')
import ev2
R = '/tmp/hl-rules-study/runs/'
pairs = [l.strip() for l in open('/tmp/hl-rules-study/scratch/included.txt') if l.strip()]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)
rows = []
for p in pairs:
    wd = R + p; al = wd + '/align'
    ta = cv2.imread(al + '/al_0001.tif', cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]), cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5: Ra = Ra / np.iinfo(mk.dtype).max
    noT = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk(5)) > 0
    clip = np.load(wd + '/masks.npz')['F'].astype(np.float32).max(2) > 0
    m = clip & ~noT
    if not m.any(): continue
    r = {'pair': p.split('/')[-1]}
    for off in (0, -1, -2, -3):
        lin = np.maximum(T[m] * 2.0 ** off, 0)
        a = lin @ ev2.TO_SRGB.T
        over = a > 1
        part = over.any(1) & ~over.all(1)
        bright = lin.max(1) > 1       # over white already in linear Rec2020
        neg = (a < 0).any(1)          # outside the sRGB gamut
        r[off] = (over.any(1).mean(), over.all(1).mean(), (part & bright).mean(), (part & ~bright).mean(), neg.mean())
    rows.append(r)
for off in (0, -1, -2, -3):
    anyv = np.array([r[off][0] for r in rows]); allv = np.array([r[off][1] for r in rows])
    part = anyv - allv
    print('%+d EV: pixels with 1-2 channels over white (hue distorted): median %.1f%%, mean %.1f%%, max %.1f%%; all 3 over (white): median %.1f%%'
          % (off, 100 * np.median(part), 100 * part.mean(), 100 * part.max(), 100 * np.median(allv)))
    worst = sorted(rows, key=lambda r: -(r[off][0] - r[off][1]))[:6]
    print('   of the hue-distorted: Rec2020 already over white %.1f%% (mean share of pixels), only after the sRGB matrix %.1f%%; outside the sRGB gamut (a channel < 0): %.1f%%'
          % (100 * np.mean([r[off][2] for r in rows]), 100 * np.mean([r[off][3] for r in rows]), 100 * np.mean([r[off][4] for r in rows])))
    print('   worst:', ', '.join('%s %.0f%%' % (r['pair'][:28], 100 * (r[off][0] - r[off][1])) for r in worst))
