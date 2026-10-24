import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp = d['inp']
pmax = np.array([2.0390625, 1.0, 1.21875])
# holes: unflagged pixels whose 4 neighbors are all flagged, as hlcolor would flag at threshold t
for t in (0.98, 0.96, 0.95, 0.93, 0.90):
    f = (inp >= t * pmax).any(-1)
    nb = np.zeros(f.shape, np.int8)
    nb[1:, :] += f[:-1, :]; nb[:-1, :] += f[1:, :]; nb[:, 1:] += f[:, :-1]; nb[:, :-1] += f[:, 1:]
    border = (~f) & (nb > 0)
    print('threshold %.2f: flagged %8d, border %7d, holes %7d' % (t, f.sum(), border.sum(), ((~f) & (nb == 4)).sum()))
