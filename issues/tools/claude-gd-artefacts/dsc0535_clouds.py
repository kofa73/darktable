import sys
import numpy as np
import colour
from PIL import Image
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
st = np.rot90(d['st'], 1)[:3008]
# flag classes sampled at export pixels
s = 4012 / 2000
ys, xs = np.mgrid[850:1350, 0:900]
cls = st[(ys * s).astype(int), (xs * s).astype(int)]
E = '/tmp/gradient-domain-eval/raw/darktable_exported/DSC_0535_'
labs = {}
for n in ('no_hl_reconstruction', 'segmentation_based', 'gradient_domain'):
    rgb = np.asarray(Image.open(E + n + '.jpg').convert('RGB'), np.float32)[850:1350, 0:900] / 255
    labs[n] = colour.XYZ_to_Lab(colour.sRGB_to_XYZ(rgb))
print('bottom-left clouds, export 0-900 x 850-1350; mean L* a* b* per flag class (bit 0 R, 1 G, 2 B)')
for c in range(8):
    m = cls == c
    if m.mean() < 0.005: continue
    print(' class %d (%4.1f%%): ' % (c, 100 * m.mean()) + '  '.join('%s %5.1f %5.1f %5.1f' % ((n[:6],) + tuple(labs[n][m].mean(0))) for n in labs))
