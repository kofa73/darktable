# plan_tz.py <out.json>: synthetic clipping plan for the TZ101 brackets (ids t..) of
# bracketed/selection.tsv, as plan41.json: the darkest frame, raised by k EV so that 5% of
# its photosites reach 0.98 of the white level (ev2.py's flag), k from the 95th percentile
# of the black-subtracted, normalized raw values.
import sys, json, os
import numpy as np, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from bracket_scan import white_level
from ev2 import model
B = '/tmp/gradient-domain-eval/bracketed'
out = []
for line in open(os.path.join(B, 'selection.tsv')):
    f = line.rstrip('\n').split('\t')
    d = f[0]
    if not d.split('/')[-1].startswith('t') or d.split('/')[0] not in ('city', 'indoor', 'night', 'colored'):
        continue
    raw = os.path.join(B, d, f[4])
    cam = model(raw)
    with rawpy.imread(raw) as r:
        a = r.raw_image_visible.astype(np.float32)
        black = np.array(r.black_level_per_channel, np.float32)[r.raw_colors_visible]
        wl = white_level(cam, r)
        a = (a - black) / (wl - black)
    q = float(np.percentile(a, 95))
    k = round(float(np.log2(0.98 / q)), 2)
    out.append(dict(name='syn_' + d.split('/')[-1], kind='synth', ref=raw, clipped=raw, k=k,
                    already=float((a >= 0.98).mean()), camera=cam, white=float(wl)))
    print(out[-1]['name'], k, '%.5f' % out[-1]['already'], cam, wl, flush=True)
json.dump(out, open(sys.argv[1], 'w'), indent=1)
