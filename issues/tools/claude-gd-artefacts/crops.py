# crops.py <ev2 workdir> <out.jpg> [methods...]: side by side crops of the truth and
# the methods' tone-mapped PNGs, around the window where the first method's error
# against the truth, in the 'all' mask, is largest
import sys, numpy as np, cv2
from PIL import Image
wd, out = sys.argv[1], sys.argv[2]
methods = sys.argv[3:] or ['opposed', 'gd', 'gd-wb4x2']
import os
# CROP_SUFFIX=_d crops the display-referred renders of EV2_DISPLAY
suf = os.environ.get('CROP_SUFFIX', '')
L = lambda n: np.asarray(Image.open('%s/%s%s.png' % (wd, n, suf))).astype(np.float32)
t = L('truth')
import os
m = np.load(wd + '/masks.npz')[os.environ.get('CROP_MASK', 'all')]
e = np.abs(L(methods[1] if len(methods) > 1 else methods[0]) - t).sum(2) * m
H, W = m.shape
s = min(400, H // 2, W // 2)
k = cv2.blur(e, (s, s))
y, x = np.unravel_index(k.argmax(), k.shape)
y0, x0 = min(max(0, y - s // 2), H - s), min(max(0, x - s // 2), W - s)
tiles = [t] + [L(n) for n in methods]
row = np.concatenate([a[y0:y0 + s, x0:x0 + s] for a in tiles], 1).astype(np.uint8)
Image.fromarray(row).save(out, quality=92)
print('crop', x0, y0, s, 'order: truth', ' '.join(methods))
