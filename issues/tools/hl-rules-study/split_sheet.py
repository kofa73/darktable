# split_sheet.py <sheet.png>...: cuts a tall crop sheet into <name>-part<n>.png of at most SP_HEIGHT
# (default 1200) px, at the white separators between crop rows so no row is cut in half. Only runs
# of at least 6 white rows count (crop_sheet.py's separator): the label bar above each row has shorter
# white runs below its text, and a cut there would part a label from its row.
import sys, os
import numpy as np, cv2

H = int(os.environ.get('SP_HEIGHT', '1200'))
for path in sys.argv[1:]:
    img = cv2.imread(path)
    w = np.r_[False, (img == 255).all(axis=(1, 2)), False].astype(np.int8)
    d = np.diff(w)
    starts, ends = np.where(d == 1)[0], np.where(d == -1)[0]
    # a cut after a separator run: the next part starts with the label bar
    seps = np.array([e for s, e in zip(starts, ends) if e - s >= 6], int)
    cuts, start = [], 0
    while img.shape[0] - start > H:
        ok = seps[(seps > start) & (seps <= start + H)]
        end = int(ok[-1]) if len(ok) else start + H
        cuts.append((start, end))
        start = end
    cuts.append((start, img.shape[0]))
    base = os.path.splitext(path)[0]
    for i, (a, b) in enumerate(cuts):
        cv2.imwrite('%s-part%d.png' % (base, i), img[a:b])
    print(path, len(cuts), 'parts')
