# dome_clip.py <workdir>...: issue score-display-clipping, for the dome crop sheets (issue
# dome-for-clipped-cores): in the fully clipped pixels (masks.npz F min >= 0.5), the share whose
# render, shown as crop_sheet.py does (lin_eval.to_srgb8 at -2 and -4 EV), has 1-2 sRGB channels
# over white (hue shifted) or all 3 (white), per method.
import sys, os
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts'); sys.path.insert(0, '/tmp/hl-rules-study/scripts')
import ev2
from pfm import read_pfm
import json
M = ['opposed', 'gd-r-hold', 'gd-r-dome', 'gd-r-domelin']
acc = {(m, o): [] for m in M for o in (-2, -4)}
for wd in sys.argv[1:]:
    F = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32)
    full = F.min(2) >= 0.5
    if not full.any():
        continue
    s = 2.0 ** (json.load(open(os.path.join(wd, 'scores.json')))['k'] - ev2.E0)
    for m in M:
        x = read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32)[full] * s
        for o in (-2, -4):
            a = np.maximum(x * 2.0 ** o, 0) @ ev2.TO_SRGB.T
            over = a > 1
            acc[(m, o)].append((over.any(1).mean() - over.all(1).mean(), over.all(1).mean(), full.sum()))
for o in (-2, -4):
    print('%+d EV, fully clipped pixels, pixel-weighted over %d pairs:' % (o, len(acc[(M[0], o)])))
    for m in M:
        v = np.array(acc[(m, o)]); w = v[:, 2] / v[:, 2].sum()
        print('  %-13s 1-2 channels over white %5.1f%%   all 3 over white %5.1f%%' % (m, 100 * (v[:, 0] * w).sum(), 100 * (v[:, 1] * w).sum()))
