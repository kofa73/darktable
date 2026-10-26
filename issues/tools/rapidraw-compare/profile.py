# radial luminance profiles outside the fully clipped regions of the Laneway raw, on 2x2 quads:
# does a lamp's glow fall off gradually while a window or sign drops to a dark frame?
import sys
import numpy as np, rawpy, cv2
raw = rawpy.imread('/tmp/gradient-domain-eval/iain/Laneway bracket set clipped.NEF')
img = raw.raw_image_visible.astype(np.float32)
cols = raw.raw_colors_visible
black = np.array(raw.black_level_per_channel, np.float32)[cols]
white = 3880.0  # rawprepare white point in the XMP (0x0f28), as darktable uses it
v = (img - black) / (white - black.mean())
wb = np.array(raw.camera_whitebalance[:3], np.float32); wb /= wb[1]
h, w = v.shape[0] // 2 * 2, v.shape[1] // 2 * 2
v, cols = v[:h, :w], cols[:h, :w]
cols = np.where(cols == 3, 1, cols)
clip = v >= 0.987
q = np.zeros((h // 2, w // 2, 3), np.float32); n = np.zeros_like(q)
qc = np.zeros((h // 2, w // 2), np.int32)
for dy in (0, 1):
    for dx in (0, 1):
        c = cols[dy::2, dx::2]; x = v[dy::2, dx::2]
        for ch in range(3):
            m = c == ch
            q[..., ch] += np.where(m, x * wb[ch], 0); n[..., ch] += m
        qc += clip[dy::2, dx::2]
q /= np.maximum(n, 1)
Y = 0.2627 * q[..., 0] + 0.6780 * q[..., 1] + 0.0593 * q[..., 2]
full = (qc == 4).astype(np.uint8)
nl, lab, st, cen = cv2.connectedComponentsWithStats(full, connectivity=8)
H, W = full.shape
print('quad grid', W, H)
big = [i for i in range(1, nl) if st[i, 4] >= 150]
big.sort(key=lambda i: -st[i, 4])
dist_out = cv2.distanceTransform((1 - full).astype(np.uint8), cv2.DIST_L2, 5)
radii = [1, 2, 4, 8, 16, 32]
for i in big[:12]:
    x, y, ww, hh, a = st[i]
    reg = (lab == i).astype(np.uint8)
    d = cv2.distanceTransform(1 - reg, cv2.DIST_L2, 5)
    prof = []
    lo = 0
    for r in radii:
        ring = (d > lo) & (d <= r) & (full == 0)
        prof.append(np.median(Y[ring]) if ring.any() else np.nan); lo = r
    # export coordinates (2000 px wide export of a W-quad-wide raw)
    s = 2000 / W
    print('region %4d area %6d quads  export box x %4d-%4d y %4d-%4d  ring Y (r<=1,2,4,8,16,32 quads): %s'
          % (i, a, x * s, (x + ww) * s, y * s, (y + hh) * s, ' '.join('%.3f' % p for p in prof)))
