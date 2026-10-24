# bracket_pick.py <scan2.json> [min_agreement]
# Summarizes bracket_scan2.py output, one line per group:
#  - d0clip: share of quads the darkest frame clips; artificial clipping needs
#    it at most 0.005, as for plan41.json
#  - best real pair: darker frame i holds the truth where brighter frame j
#    clips 0.5% to 15% of the quads; i clips at most 1% of that region; the
#    exposure ratio varies by at most 0.25 EV over the image; j is not
#    blurred against i (sharpness ratio at least 0.9, measured only between
#    frames of one f-number, because depth of field changes it too); among
#    those, the highest clip agreement, then the gap closest to 3 EV
#  - blur: the smallest sharpness ratio between consecutive frames of one
#    f-number, the frame where it happens, and its shutter speed
import sys, json

res = json.load(open(sys.argv[1]))
min_agree = float(sys.argv[2]) if len(sys.argv) > 2 else 0.8
for k, g in enumerate(res):
    if 'error' in g:
        print('b%02d  ERROR %s' % (k, g['error']))
        continue
    fr = g['frames']
    best = None
    for p in g['pairs']:
        if not (0.005 <= p['j_clipped'] <= 0.15):
            continue
        if p.get('i_clipped_in_region', 1) > 0.01 or p['ev_spread'] is None or p['ev_spread'] > 0.25:
            continue
        if p['same_f'] and (p['sharp_j_vs_i'] or 0) < 0.9:
            continue
        key = (p.get('clip_agreement', 0), -abs(p['ev'] - 3))
        if best is None or key > best[0]:
            best = (key, p)
    blur = None
    for p in g['pairs']:
        if p['same_f'] and p['j'] == p['i'] + 1 and p['sharp_j_vs_i'] is not None:
            if blur is None or p['sharp_j_vs_i'] < blur['sharp_j_vs_i']:
                blur = p
    s = 'b%02d %s d0clip %.4f maxclip %.3f' % (k, g['time'][5:16], fr[0]['clipped'], fr[-1]['clipped'])
    if best:
        p = best[1]
        s += ' | pair %d>%d ev %.2f agree %.2f spread %.2f sharp %s jclip %.3f deep %.2f col %.2f shift %s' % (
            p['i'], p['j'], p['ev'], p['clip_agreement'], p['ev_spread'],
            '%.2f' % p['sharp_j_vs_i'] if p['sharp_j_vs_i'] else '-', p['j_clipped'], p['deep'],
            p.get('colored', 0), p['shift'])
        if p['clip_agreement'] < min_agree:
            s += ' LOW'
    else:
        s += ' | no pair'
    if blur:
        s += ' | blur %.2f at %d (%s)' % (blur['sharp_j_vs_i'], blur['j'], fr[blur['j']]['shutter'])
    print(s)
