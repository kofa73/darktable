# e_bound.py <run dir>: upper bound for rule E (scene white level). E decides per image
# (one level L per image); with the true L its three bands give hue fill, white fill
# (capped) or hue fill for all fully clipped regions of the image. Any per-image rule,
# E with a perfect L included, scores at best as the per-image oracle choice between the
# whole-image fills, chosen here by the truth (minimum dE on the fully clipped mask).
# Printed: rescues of that oracle (hue/white, and hue/white/edge) next to the fixed fills,
# for all images and for each test half of the Phase 2 splits.
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze as A


def per_image_oracle(imgs, fills):
    ch = []
    for im in imgs:
        best = None
        for c in fills:
            g = A.composite(im, {None: c})['full']
            if np.isfinite(g) and (best is None or g < best[0]):
                best = (g, c)
        ch.append({None: best[1] if best else 0})
    return A.rescues(imgs, ch)


for st in ('model', 'cont'):
    imgs = A.load(sys.argv[1], st)
    for name, sub in (('all', imgs), ('fc+iain', [i for i in imgs if i['source'] in A.HALVES['fc+iain']]),
                      ('si+syn', [i for i in imgs if i['source'] in A.HALVES['si+syn']])):
        print('-- set %s, %s (%d images)' % (st, name, len(sub)))
        for c, f in enumerate(A.FILLS):
            print('  fixed %-6s %s' % (f, A.fmt(A.fixed_fill(sub, c))))
        print('  per-image oracle hue/white      %s' % A.fmt(per_image_oracle(sub, (0, 1))))
        print('  per-image oracle hue/white/edge %s' % A.fmt(per_image_oracle(sub, (0, 1, 2))))
        print('  per-zone oracle                 %s' % A.fmt(A.oracle(sub, st)))
