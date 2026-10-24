# pick_cores.py <out plan.json> <bracket dir>...: for scenes without a 1-2 EV pair (pick_pairs.py),
# a pair for the crop sheet only (crop_sheet.py), not for scores: the middle frame by median raw
# value as the clipped frame, the darkest as the truth, so that the crop sheet shows the pixels that
# no frame records at the exposure Kofa would normally develop. Exposure gap as pick_pairs.py.
import sys, os, glob, json
import numpy as np, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

out, dirs = sys.argv[1], sys.argv[2:]
plan = []
for d in map(os.path.abspath, dirs):
    fr = {}
    for f in sorted(sum((glob.glob(os.path.join(d, '*.' + x)) for x in ('RW2', 'NEF', 'dng', 'DNG')), [])):
        cam = ev2.model(f)
        with rawpy.imread(f) as r:
            v = r.raw_image_visible.astype(np.float32)
            b = np.array(r.black_level_per_channel, np.float32)[r.raw_colors_visible]
            fr[f] = (v - b) / (ev2.white_level(cam, r) - b)
    names = sorted(fr, key=lambda f: np.median(fr[f]))
    c, t = names[len(names) // 2], names[0]
    m = (fr[t] > 0.01) & (fr[t] < 0.25) & (fr[c] < 0.9)
    k = float(np.log2(np.median(fr[c][m] / fr[t][m])))
    scene = os.path.basename(d.rstrip('/'))
    print('%s: %s vs truth %s, gap %.2f EV, clipped %.3f%%, truth %.3f%%' % (
        scene, os.path.basename(c), os.path.basename(t), k, 100 * (fr[c] >= 0.98).mean(), 100 * (fr[t] >= 0.98).mean()))
    plan.append(dict(name='%s-%s-%s' % (scene, os.path.basename(c)[-8:-4], os.path.basename(t)[-8:-4]),
                     kind='pair', ref=t, clipped=c, k=round(k, 3)))
json.dump(plan, open(out, 'w'), indent=1)
