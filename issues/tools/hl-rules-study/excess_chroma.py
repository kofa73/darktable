# excess_chroma.py <workdir>...: "added color" on ev2 display renders: within DIL px of the clipped
# pixels, the share of pixels whose CIE LCh chroma (display sRGB -> Lab D65) exceeds the truth's
# by more than DC, and the mean of max(0, C_method - C_truth). Targets the failure Kofa sees:
# colored blotches where the truth is neutral, against a bland but plausible cast.
import sys, os
import numpy as np, cv2, colour
DIL, DC = 25, 8.0
def lab(p):
    s = cv2.cvtColor(cv2.imread(p), cv2.COLOR_BGR2RGB).astype(np.float32) / 255
    return colour.XYZ_to_Lab(colour.sRGB_to_XYZ(s))
for wd in sys.argv[1:]:
    mk = np.load(os.path.join(wd, 'masks.npz'))
    clip = (mk['F'].astype(np.float32) > 0).any(2) | mk['all']
    reg = cv2.dilate(clip.astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * DIL + 1,) * 2)) > 0
    t = lab(os.path.join(wd, 'truth_d.png')); Ct = np.hypot(t[..., 1], t[..., 2])
    out = []
    for m in ('opposed', 'gd-wb4x10'):
        x = lab(os.path.join(wd, m + '_d.png')); d = np.hypot(x[..., 1], x[..., 2]) - Ct
        out.append('%s %5.2f%% %5.2f' % (m, 100 * (d[reg] > DC).mean(), np.maximum(d[reg], 0).mean()))
    print('%-28s %s' % (os.path.basename(wd.rstrip('/')), '   '.join(out)))
