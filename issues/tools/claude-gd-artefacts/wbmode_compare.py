# wbmode_compare.py <dir> <ev> <out.png> [x y w h]
# Compares the renders <name>-late.pfm ("as shot to reference") and <name>-d65.pfm
# ("camera reference") of wbmode/. Renders are linear Rec2020 without a tone mapper.
# Display: scale by 2^ev, then rgb / (1 + Y), which keeps the chromaticity, then sRGB.
# Statistics cover the pixels that some reconstruction changed against none-late.pfm.
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pfm import read_pfm
from PIL import Image, ImageDraw

d, ev, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
crop = tuple(map(int, sys.argv[4:8])) if len(sys.argv) > 4 else None
names = ['none', 'opposed', 'segments', 'gd-basic', 'gd-wb4-x7', 'gd-wb4-x11', 'gd-wb4-x14']
labels = {'none': 'no reconstruction', 'opposed': 'inpaint opposed', 'segments': 'segmentation based',
          'gd-basic': 'gradient domain, basic',
          'gd-wb4-x7': 'hue from measured channels, hue estimate in fully clipped',
          'gd-wb4-x11': 'continuous border, white in fully clipped',
          'gd-wb4-x14': 'continuous border, edge hue to white in fully clipped'}
Y2020 = np.array([0.2627, 0.6780, 0.0593])
M = np.array([[1.6605, -0.5876, -0.0728], [-0.1246, 1.1329, -0.0083], [-0.0182, -0.1006, 1.1187]])  # 2020 -> 709

def disp(a):
    a = a * 2.0 ** ev
    y = (a @ Y2020)[..., None]
    a = a / (1.0 + np.maximum(y, 0))
    a = np.clip(a @ M.T, 0, 1)
    s = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    return (s * 255 + 0.5).astype(np.uint8)

img = {(n, m): read_pfm(os.path.join(d, '%s-%s.pfm' % (n, m))) for n in names for m in ('late', 'd65')}
none = img[('none', 'late')]
changed = np.zeros(none.shape[:2], bool)
for (n, m), a in img.items():
    if n != 'none' and a.shape == none.shape:
        changed |= np.abs(a - none).max(-1) > 1e-3 * max(1e-6, float(none.max()))
print('changed pixels: %d of %d' % (changed.sum(), changed.size))
print('%-20s %-5s %8s %8s %8s %8s' % ('method', 'mode', 'Y', 'r', 'g', 'b'))
for n in names:
    for m in ('late', 'd65'):
        a = img[(n, m)][changed]
        y = a @ Y2020
        rgb = a.mean(0) / max(1e-9, a.mean(0).sum())
        print('%-20s %-5s %8.3f %8.3f %8.3f %8.3f' % (n, m, y.mean(), *rgb))

tiles = []
for n in names:
    row = []
    for m in ('late', 'd65'):
        a = img[(n, m)]
        if crop: x, y0, w, h = crop; a = a[y0:y0 + h, x:x + w]
        t = Image.fromarray(disp(a))
        ImageDraw.Draw(t).text((6, 4), '%s, %s' % (labels[n], 'as shot to reference' if m == 'late' else 'camera reference'),
                               fill=(255, 255, 0))
        row.append(t)
    tiles.append(row)
w, h = tiles[0][0].size
sheet = Image.new('RGB', (2 * w + 4, len(tiles) * (h + 4)), (40, 40, 40))
for i, row in enumerate(tiles):
    for j, t in enumerate(row):
        sheet.paste(t, (j * (w + 4), i * (h + 4)))
sheet.save(out)
print('wrote', out, sheet.size)
