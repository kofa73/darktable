import sys
import numpy as np, colour
from PIL import Image
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
# Lab comparison of renders: profile across the tower's right edge, and mean color per flag class in two areas
E = '/tmp/gradient-domain-eval/raw/darktable_exported/DSC_0535_'
renders = {'C basic': '../v2/r-gd-basic.jpg', 'py orig': 'r-orig.jpg', 'along': 'r-corr-along.jpg',
           'line': 'r-corr-line.jpg', 'segment.': E + 'segmentation_based.jpg', 'opposed': E + 'inpaint_opposed.jpg'}
labs = {}
for k, p in renders.items():
    rgb = np.asarray(Image.open(p).convert('RGB'), np.float32) / 255
    labs[k] = colour.XYZ_to_Lab(colour.sRGB_to_XYZ(rgb))
xs = list(range(1140, 1400, 20))
print('a* / b* across the right edge of the tower (export rows 1150-1250), x = ' + ' '.join('%d' % x for x in xs))
for k, L in labs.items():
    band = L[1150:1250, :1400].mean(0)
    print('  %-9s a* %s' % (k, ' '.join('%5.1f' % band[x, 1] for x in xs)))
    print('  %-9s b* %s' % ('', ' '.join('%5.1f' % band[x, 2] for x in xs)))
st = np.rot90(hlcdump.load('../v2/gd.dump')['st'], 1)[:3008]
s = 4012 / 2000
for area, (y0, y1, x0, x1) in {'bottom-left clouds': (850, 1350, 0, 900), 'upper sky': (150, 650, 300, 1500)}.items():
    ys, xx = np.mgrid[y0:y1, x0:x1]
    cls = st[(ys * s).astype(int), (xx * s).astype(int)]
    print('%s: mean L* a* b* per flag class' % area)
    for c in range(8):
        m = cls == c
        if m.mean() < 0.005: continue
        print('  class %d (%4.1f%%) ' % (c, 100 * m.mean()) + ' | '.join('%s %4.1f %5.1f %5.1f' % ((k[:5],) + tuple(L[y0:y1, x0:x1][m].mean(0))) for k, L in labs.items()))
