import sys
import numpy as np
from PIL import Image
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp, st, pre = d['inp'], d['st'], d['pre']
g = inp[..., 1]
# where does the green sit in the flagged region: a plateau means sensor saturation
edges = np.array([0.98, 0.99, 1.0, 1.01, 1.02, 1.03, 1.04, 1.05, 1.055, 1.06, 1.07, 1.09])
hist, _ = np.histogram(g[(st & 2) > 0], edges)
for a, b, n in zip(edges[:-1], edges[1:], hist):
    print('G in [%.3f, %.3f): %8d' % (a, b, n))
# same for red and blue relative to their clipping level
for c, cl in ((0, 2.0390625 * 0.98), (2, 1.21875 * 0.98)):
    v = inp[..., c][(st >> c) & 1 == 1] / (cl / 0.98)
    print('chan', c, 'flagged / pmax: p1 %.3f p50 %.3f p99 %.3f max %.3f' % tuple(np.percentile(v, [1, 50, 99, 100])))
# false-color map of the flag classes, downscaled 4x
col = np.array([[0,0,0],[255,0,0],[0,255,0],[255,255,0],[0,0,255],[255,0,255],[0,255,255],[255,255,255]], np.uint8)
Image.fromarray(col[st[::4, ::4]]).save('state.png')
# green above sensor saturation of a 12-bit raw with white point 3880
m = (g >= 4095 / 3880 - 0.002)
Image.fromarray((m[::4, ::4] * 255).astype(np.uint8)).save('g_sat.png')
print('G >= 1.053:', int(m.sum()))
