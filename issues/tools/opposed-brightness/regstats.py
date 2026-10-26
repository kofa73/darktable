# regstats.py: per scored pair, image size and the fully clipped regions (8-connected, F min >= 0.5)
import sys, os, numpy as np, cv2
R = '/tmp/hl-rules-study/runs/'
pairs = [l.strip() for l in open('/tmp/hl-rules-study/scratch/included.txt') if l.strip()]
tot = []
for p in pairs:
    F = np.load(R + p + '/masks.npz')['F']
    full = (F.min(2) >= 0.5).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(full, 8)
    a = np.sort(st[1:, 4])[::-1]
    tot.append(a.max() if a.size else 0)
    print('%-40s %5dx%-5d full %6.2f%%  regions %5d  largest %s' % (p.split('/')[-1][:40], F.shape[1], F.shape[0], 100 * full.mean(), n - 1, a[:4].tolist()))
t = np.array(tot); print('largest region > 50000:', (t > 50000).sum(), '> 500000:', (t > 500000).sum())
