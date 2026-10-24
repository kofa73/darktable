import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp, st = d['inp'], d['st']
pmax = np.array([2.0390625, 1.0, 1.21875])
# values in raw units (white point 3880), whole image, per channel
for c in range(3):
    v = inp[..., c].ravel() / pmax[c] * 3880
    hist, edges = np.histogram(v, np.arange(3600, 4200, 20))
    print('chan', c)
    for a, n in zip(edges[:-1], hist):
        print('  %4d-%4d %9d %s' % (a, a + 20, n, '#' * int(60 * n / hist.max())))
