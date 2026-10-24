# wbmode_clipped.py <dir> <name> [<name> ...]
# Mean color of the fully clipped pixels in <name>-late.pfm ("as shot to reference")
# and <name>-d65.pfm ("camera reference"), as R/G and B/G in linear Rec2020, where
# neutral is 1, 1. Fully clipped pixels are found in none-late.pfm: without
# reconstruction they all hold the same color, the most frequent one among the
# brighter pixels. The tolerance of 0.5% leaves out pixels that the export
# downscale mixed with others. Partly clipped pixels are the others that inpaint
# opposed or basic gradient domain change by more than 1% in "as shot to reference".
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pfm import read_pfm

d, names = sys.argv[1], sys.argv[2:]
Y = np.array([0.2627, 0.6780, 0.0593])
none = read_pfm(os.path.join(d, 'none-late.pfm'))
y = none @ Y
bright = none[y > 0.1 * y.max()]
colors, counts = np.unique(np.round(bright, 3), axis=0, return_counts=True)
clip_color = colors[counts.argmax()]
mask = (np.abs(none - clip_color) <= 0.005 * clip_color).all(-1)
partly = np.zeros_like(mask)
for ref in ('opposed', 'gd-basic'):
    f = os.path.join(d, '%s-late.pfm' % ref)
    if os.path.exists(f):
        partly |= (np.abs(read_pfm(f) - none) > 0.01 * np.maximum(none, 1e-3)).any(-1)
partly &= ~mask
print('clip color %s, fully clipped pixels: %d, partly clipped pixels: %d, of %d'
      % (np.round(clip_color, 3), mask.sum(), partly.sum(), mask.size))
for label, region in (('fully clipped', mask), ('partly clipped', partly)):
    if not region.any(): continue
    print('%s:' % label)
    print('  %-12s %-28s   %-28s' % ('', 'as shot to reference', 'camera reference'))
    for n in names:
        out = []
        for m in ('late', 'd65'):
            a = read_pfm(os.path.join(d, '%s-%s.pfm' % (n, m)))[region].mean(0)
            out.append('R/G %5.2f  B/G %5.2f  Y %6.3f' % (a[0] / a[1], a[2] / a[1], a @ Y))
        print('  %-12s %-28s   %-28s' % (n, out[0], out[1]))
