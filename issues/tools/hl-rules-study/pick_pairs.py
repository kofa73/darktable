# pick_pairs.py <out plan.json> <bracket dir>...: real pairs from Kofa's brackets with 1-2 EV of
# overexposure. Per frame: median normalized raw value and the share of photosites at or above 0.98
# of the camera's white level (ev2.white_level, as the scores use). Pairs (clipped, truth) with an
# exposure gap of 0.8 to 2.3 EV (median ratio of photosites both frames measure well), clipped
# frame with >= 0.1% clipped photosites, truth with less than a third of that. Per scene the pair
# with the most clipping; a second pair if one exists with a gap differing by >= 0.8 EV.
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
        # truncated raws exist (g289 P1080098.RW2): skip the frame, not the whole run
        try:
            with rawpy.imread(f) as r:
                v = r.raw_image_visible.astype(np.float32)
                b = np.array(r.black_level_per_channel, np.float32)[r.raw_colors_visible]
                fr[f] = (v - b) / (ev2.white_level(cam, r) - b)
        except rawpy.LibRawError as ex:
            print('  %-40s unreadable, skipped: %s' % (os.path.basename(f), ex))
    names = list(fr)
    rows = []
    for c in names:
        for t in names:
            if c == t:
                continue
            m = (fr[t] > 0.01) & (fr[t] < 0.25) & (fr[c] < 0.9)
            if m.sum() < 1000:
                continue
            k = float(np.log2(np.median(fr[c][m] / fr[t][m])))
            cc, tc = float((fr[c] >= 0.98).mean()), float((fr[t] >= 0.98).mean())
            if 0.8 <= k <= 2.3 and cc >= 1e-3 and tc < cc / 3:
                rows.append((cc, k, c, t, tc))
    scene = os.path.basename(d.rstrip('/'))
    for f in names:
        print('  %-40s median %.4f clipped %.3f%%' % (os.path.basename(f), np.median(fr[f]), 100 * (fr[f] >= 0.98).mean()))
    rows.sort(reverse=True)
    picked = []
    for r in rows:
        if not picked or (len(picked) == 1 and abs(r[1] - picked[0][1]) >= 0.8):
            picked.append(r)
    if not picked:
        print('%s: no pair' % scene)
    for cc, k, c, t, tc in picked:
        print('%s: %s vs truth %s, gap %.2f EV, clipped %.2f%%, truth %.3f%%' % (scene, os.path.basename(c), os.path.basename(t), k, 100 * cc, 100 * tc))
        # clipped and truth frame numbers: two pairs can share the clipped frame (t63)
        plan.append(dict(name='%s-%s-%s' % (scene, os.path.basename(c)[-8:-4], os.path.basename(t)[-8:-4]),
                         kind='pair', ref=t, clipped=c, k=round(k, 3)))
json.dump(plan, open(out, 'w'), indent=1)
print(len(plan), 'pairs')
