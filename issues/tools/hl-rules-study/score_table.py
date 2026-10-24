# score_table.py <run dir>: per image k and mean dE on the partly clipped and all masks for
# inpaint opposed, segmentation based, continuous border + hue fill (value 10) and opposed's
# estimate as hue (value 9), sorted by k
import json, glob, os, sys
rows = []
for f in glob.glob(os.path.join(sys.argv[1], '*', 'scores.json')):
    s = json.load(open(f))
    m = s['methods']
    o = lambda k, mk: m[k][mk]['dE_mean'] if mk in m.get(k, {}) else float('nan')
    rows.append((os.path.basename(os.path.dirname(f)), s.get('k', float('nan')), o('opposed', 'partial'),
                 o('segments', 'partial'), o('gd-wb4x10', 'partial'), o('gd-wb4x9', 'partial'),
                 o('opposed', 'all'), o('gd-wb4x10', 'all')))
rows.sort(key=lambda r: r[1])
print('name, k | partly clipped: inpaint opposed, segmentation based, continuous border hue fill, '
      "opposed's estimate | all: inpaint opposed, continuous border hue fill")
for r in rows:
    print('%-34s %5.2f | %5.1f %5.1f %5.1f %5.1f | %5.1f %5.1f' % r)
