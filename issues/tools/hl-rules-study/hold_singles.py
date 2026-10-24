# hold_singles.py <outdir> <raw>...: issue holding-and-writeback, step 2. Renders single raws (no
# truth) with the methods of HS_METHODS (ev4.py names) at the default exposure E0, linear Rec2020
# PFMs, then one sheet per raw: the whole image at HS_EV for each method, and crops of the
# HS_CROPS 256 px windows where the methods differ most (largest summed max-channel difference
# between any two methods, windows not overlapping). Uses SYNTH_CFG, EV2_THREADS.
import sys, os
import numpy as np, cv2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ['EV2_KEEP_PFM'] = '1'
import ev4
from lin_eval import to_srgb8
ev2 = ev4.ev2

METHODS = os.environ.get('HS_METHODS', 'gd-wb4x10,gd-nohold,gd-nohold-wb0,gd-r-hold,gd-r-nohold').split(',')
NCROP, W = int(os.environ.get('HS_CROPS', '3')), 256
# display gain (2^-1 = -1 EV by default) and extra crop centers 'x,y;x,y' in render pixels
GAIN = 2.0 ** float(os.environ.get('HS_EV', '-1'))
EXTRA = [tuple(int(v) for v in c.split(',')) for c in os.environ.get('HS_EXTRA', '').split(';') if c]
out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def label(img, s):
    img = np.ascontiguousarray(img)
    cv2.putText(img, s, (4, img.shape[0] - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 0), 3, cv2.LINE_AA)
    cv2.putText(img, s, (4, img.shape[0] - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
    return img


for raw in sys.argv[2:]:
    name = os.path.splitext(os.path.basename(raw))[0].replace(' ', '_')
    imgs = {}
    for m in METHODS:
        pfm = os.path.join(out, '%s_%s.pfm' % (name, m))
        imgs[m] = ev2.read_pfm(pfm) if os.path.exists(pfm) else ev2.render(raw, pfm, m, ev2.E0, None, 1600)
        print(name, m, '%.0f s' % ev2.TIMES.get(os.path.basename(pfm), 0), flush=True)
    # where the methods differ most
    diff = np.zeros(imgs[METHODS[0]].shape[:2], np.float32)
    for a in METHODS:
        for b in METHODS:
            if a < b:
                diff = np.maximum(diff, np.abs(imgs[a] - imgs[b]).max(2))
    box = cv2.boxFilter(diff, -1, (W, W), normalize=False)
    crops = []
    for _ in range(NCROP):
        y, x = np.unravel_index(np.argmax(box), box.shape)
        if box[y, x] <= 0:
            break
        y0, x0 = max(0, min(y - W // 2, diff.shape[0] - W)), max(0, min(x - W // 2, diff.shape[1] - W))
        crops.append((y0, x0))
        box[max(0, y0 - W):y0 + 2 * W, max(0, x0 - W):x0 + 2 * W] = 0
    for x, y in EXTRA:
        crops.append((max(0, min(y - W // 2, diff.shape[0] - W)), max(0, min(x - W // 2, diff.shape[1] - W))))
    rows = []
    small = [label(cv2.resize(to_srgb8(imgs[m] * GAIN), None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA), m) for m in METHODS]
    rows.append(np.concatenate(small, 1))
    for y0, x0 in crops:
        rows.append(np.concatenate([label(to_srgb8(imgs[m][y0:y0 + W, x0:x0 + W] * GAIN), '%s (%d,%d)' % (m, x0, y0)) for m in METHODS], 1))
    width = max(r.shape[1] for r in rows)
    rows = [np.concatenate([r, np.full((r.shape[0], width - r.shape[1], 3), 255, np.uint8)], 1) for r in rows]
    cv2.imwrite(os.path.join(out, name + os.environ.get('HS_SUFFIX', '') + '-sheet.png'), cv2.cvtColor(np.concatenate(rows, 0), cv2.COLOR_RGB2BGR))
    print(name, 'sheet with', len(crops), 'crops', flush=True)
