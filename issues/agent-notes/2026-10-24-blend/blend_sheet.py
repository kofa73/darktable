# blend_sheet.py <out.png> <pair workdir>...: visual check of blend25 (issue opposed-hue-blend).
# Per workdir one row of four 360x270 crops at -1 EV, from pair_align.py's saved PNGs
# (bl-1_*.png of the last runs): reference (aligned truth where any method changed pixels, the
# clipped frame elsewhere, flat gray where there is no truth), inpaint opposed, blend25, gradient
# domain (gd-r-mg4); BS_B and BS_G choose other renders. The crop is centered where the blend
# differs most from inpaint opposed (31 px box mean of the absolute difference). Label: pair,
# SSIMULACRA 2 at -1 EV per method (pair_align-blend.json, or BS_IN).
import sys, os, json
import numpy as np, cv2
W, H = 360, 270
# BS_B, BS_G: the blend and gradient domain renders; BS_IN: the pair_align.py result to label with
BB = os.environ.get('BS_B', 'blend25')
BG = os.environ.get('BS_G', 'gd-r-mg4')
IN = os.environ.get('BS_IN', 'pair_align-blend.json')
# BS_EV: the exposure of the panels (pair_align.py's bl<EV>_*.png must exist for it)
EV = int(os.environ.get('BS_EV', '-1'))
out, wds = sys.argv[1], sys.argv[2:]
rows = []
for wd in wds:
    name = os.path.basename(wd.rstrip('/'))
    im = {m: cv2.imread(os.path.join(wd, 'bl%+d_%s.png' % (EV, m))) for m in ('ref_opposed', 'opposed', BB, BG)}
    sc = json.load(open(os.path.join(wd, IN)))
    # no truth (pair_align.py's noTruth: the truth frame clipped or not covered, dilated 5 px): the
    # reference holds the method's own pixels there, which do not count; shown flat gray
    al = os.path.join(wd, 'align')
    ta = cv2.imread(os.path.join(al, 'al_0001.tif'), cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]),
                    cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5:
        Ra = Ra / np.iinfo(mk.dtype).max
    disk = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (11, 11))
    nt = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk) > 0
    im['ref_opposed'] = im['ref_opposed'].copy()
    im['ref_opposed'][nt] = 128
    d = cv2.blur(np.abs(im[BB].astype(np.float32) - im['opposed'].astype(np.float32)).sum(2), (31, 31))
    y, x = np.unravel_index(np.argmax(d), d.shape)
    h, w = d.shape
    x0 = int(np.clip(x - W // 2, 0, max(w - W, 0))); y0 = int(np.clip(y - H // 2, 0, max(h - H, 0)))
    crops = [im[m][y0:y0 + H, x0:x0 + W] for m in im]
    row = np.full((H + 22, 4 * W + 3 * 4, 3), 255, np.uint8)
    for i, c in enumerate(crops):
        row[22:22 + c.shape[0], i * (W + 4):i * (W + 4) + c.shape[1]] = c
    label = '%s   ref | opposed %.1f | %s %.1f | %s %.1f   (%+d EV)' % (
        name[:40], sc['opposed %+d' % EV], BB, sc[BB + ' %+d' % EV], BG, sc[BG + ' %+d' % EV], EV)
    cv2.putText(row, label, (4, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 1, cv2.LINE_AA)
    rows.append(row); rows.append(np.full((8, row.shape[1], 3), 255, np.uint8))
cv2.imwrite(out, np.vstack(rows))
