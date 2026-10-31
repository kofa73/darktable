# overview.py: contact sheet of the clipped frames of all pairs (inpaint opposed render, exposure
# lowered 1 EV) to see what the scenes are; pair number in each thumbnail
import glob, os, json
import numpy as np, cv2, sys
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2
ds = sorted(glob.glob('/tmp/hl-rules-study/runs/xtrans/x*'))
th = []
for d in ds:
    sc = json.load(open(d + '/scores.json')); s = 2.0 ** (sc['k'] - ev2.E0)
    a = ev2.read_pfm(d + '/opposed.pfm').astype(np.float32) * s * 0.5
    a = np.clip(np.maximum(a, 0) @ ev2.TO_SRGB.T, 0, 1)
    a = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    a = cv2.resize(a.astype(np.float32), (300, int(300 * a.shape[0] / a.shape[1])), interpolation=cv2.INTER_AREA)
    a = (a * 255 + 0.5).astype(np.uint8)[..., ::-1].copy()
    cv2.putText(a, os.path.basename(d)[:3] + ' ' + os.path.basename(d)[-9:-5], (5, 22), 0, 0.6, (0, 255, 255), 2)
    th.append(a)
w, h = 300, max(t.shape[0] for t in th)
cols = 7
rows = (len(th) + cols - 1) // cols
S = np.zeros((rows * h, cols * w, 3), np.uint8)
for i, t in enumerate(th):
    y, x = i // cols * h, i % cols * w
    S[y:y + t.shape[0], x:x + t.shape[1]] = t
cv2.imwrite('overview.jpg', S, [cv2.IMWRITE_JPEG_QUALITY, 90])
print(S.shape)
