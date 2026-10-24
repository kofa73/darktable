import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
st = d['st']
f = st > 0
nb = np.zeros(f.shape, np.int8)
nb[1:, :] += f[:-1, :]; nb[:-1, :] += f[1:, :]; nb[:, 1:] += f[:, :-1]; nb[:, :-1] += f[:, 1:]
holes = (~f) & (nb == 4)
for py in range(2):
    for px in range(2):
        print('phase %d%d: holes %d' % (py, px, holes[py::2, px::2].sum()))
