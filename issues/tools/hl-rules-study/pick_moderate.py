# pick_moderate.py <out plan.json> <bracket dir>...: per bracket, the brightest frame whose own
# clipping is at most 0.02% of photosites (normalized raw >= 0.98), or failing that at most 0.5%
# (light sources clipped in every frame; ev2 excludes the truth's own clipping from the score),
# and the share of its photosites that clip when it is raised by 1 and 2 EV (>= 0.98 / 2^k).
# Writes a synthetic plan with k = 1 and k = 2 for that frame (name <scene>-k1, -k2), leaving out
# a k whose added clipping is below 0.05% of photosites.
import sys, os, glob, json
import numpy as np, rawpy

out, dirs = sys.argv[1], sys.argv[2:]
plan = []
for d in dirs:
    rows = []
    for f in sorted(glob.glob(os.path.join(d, '*'))):
        if os.path.splitext(f)[1].lower() not in ('.rw2', '.nef', '.cr2', '.arw', '.dng', '.orf'):
            continue
        with rawpy.imread(f) as r:
            v = r.raw_image_visible.astype(np.float32)
            b = np.array(r.black_level_per_channel, np.float32)[r.raw_colors_visible]
            w = float(r.white_level)
            x = (v - b) / (w - b)
        c = [float((x >= 0.98 / 2 ** k).mean()) for k in (0, 1, 2)]
        rows.append((np.median(x), f, c))
    ok = [r for r in rows if r[2][0] <= 2e-4] or [r for r in rows if r[2][0] <= 5e-3]
    name = os.path.basename(d.rstrip('/'))
    if not ok:
        print('%-30s no unclipped frame (%d frames)' % (name, len(rows)))
        continue
    med, f, c = max(ok, key=lambda r: r[0])
    print('%-30s %-40s clipped %.3f%%, +1 EV %.2f%%, +2 EV %.2f%%' % (name, os.path.basename(f), 100 * c[0], 100 * c[1], 100 * c[2]))
    for k in (1, 2):
        if c[k] - c[0] < 5e-4:
            continue
        plan.append(dict(name='syn_%s-k%d' % (name, k), kind='synth', ref=f, clipped=f, k=float(k)))
json.dump(plan, open(out, 'w'), indent=1)
