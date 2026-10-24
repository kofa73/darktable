import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp, st, pre, rho = d['inp'], d['st'], d['pre'], d['rho']
h, w = st.shape
print('size', w, h)
# clipping level estimate: max per channel of the input
mx = inp.reshape(-1, 3).max(0)
print('input max per channel', mx)
for c in range(3):
    v = inp[..., c][(st >> c) & 1 == 1]
    print('chan', c, 'flagged', v.size, 'min %.4f p50 %.4f max %.4f' % (v.min(), np.median(v), v.max()))
print('state classes (bit0 R, bit1 G, bit2 B):')
for s in range(1, 8):
    n = int((st == s).sum())
    if n == 0: continue
    m = st == s
    print(' class %d n %8d  in %s  solved %s  rho %s' % (s, n,
          np.round(inp[m].mean(0), 3), np.round(pre[m].mean(0), 3), np.round(rho[m].mean(0), 3)))
# how much does the final output differ from the input
out = inp.copy()
for c in range(3):
    m = (st >> c) & 1 == 1
    out[..., c][m] = np.maximum(pre[..., c][m], inp[..., c][m])
for c in range(3):
    m = (st >> c) & 1 == 1
    ratio = out[..., c][m] / inp[..., c][m]
    below = (pre[..., c][m] < inp[..., c][m]).mean()
    print('chan', c, 'out/in: p50 %.3f p90 %.3f p99 %.3f max %.3f; solution below input in %.1f%%' %
          (np.median(ratio), np.percentile(ratio, 90), np.percentile(ratio, 99), ratio.max(), 100 * below))
np.save('state.npy', st)
