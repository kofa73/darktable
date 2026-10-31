# repick.py <cap %>: per scene the pair with the most clipping up to the cap. The pairs of pick_xt.py
# take the most clipped frame, mostly +5.33 EV frames with 60-90% of the photosites clipped, which
# leaves little to reconstruct from and is not what the 103 Bayer pairs measure.
import sys, re, os, json, glob
import numpy as np, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2
cap = float(sys.argv[1]) / 100
plan = []
for d in sorted(glob.glob('/tmp/hl-rules-study/tasks/2026-10-31-xtrans/scenes/*')):
    fr = {}
    for f in sorted(glob.glob(d + '/*.RAF')):
        with rawpy.imread(f) as r:
            v = r.raw_image_visible.astype(np.float32)
            b = np.array(r.black_level_per_channel, np.float32)[r.raw_colors_visible]
            fr[f] = (v - b) / (r.white_level - b)
    rows = []
    for c in fr:
        for t in fr:
            if c == t:
                continue
            m = (fr[t] > 0.01) & (fr[t] < 0.25) & (fr[c] < 0.9)
            if m.sum() < 1000:
                continue
            k = float(np.log2(np.median(fr[c][m] / fr[t][m])))
            cc, tc = float((fr[c] >= 0.98).mean()), float((fr[t] >= 0.98).mean())
            if 0.8 <= k <= 3.0 and 1e-3 <= cc <= cap and tc < cc / 3:
                rows.append((cc, k, c, t, tc))
    rows.sort(reverse=True)
    picked = []
    for r in rows:
        if not picked or (len(picked) == 1 and abs(r[1] - picked[0][1]) >= 0.8):
            picked.append(r)
    scene = os.path.basename(d)
    if not picked:
        print(scene, 'no pair')
    for cc, k, c, t, tc in picked:
        ct = lambda f: os.path.basename(f)[-8:-4]
        print('%s: %s vs truth %s, gap %.2f EV, clipped %.2f%%, truth %.3f%%' % (scene, os.path.basename(c), os.path.basename(t), k, 100 * cc, 100 * tc), flush=True)
        plan.append(dict(name='%s-%s-%s' % (scene, ct(c), ct(t)), kind='pair', ref=os.path.realpath(t), clipped=os.path.realpath(c), k=round(k, 3)))
json.dump(plan, open('plan.json', 'w'), indent=1)
print(len(plan), 'pairs')
