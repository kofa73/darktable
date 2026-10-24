# disp_compare.py <plan.json> <disp run>... : per image, the linear score (full1) next to the
# display-referred scores. "def" is the better of opposed and segments.
import json, os, sys
plan = json.load(open(sys.argv[1])); runs = sys.argv[2:]
def load(run, n):
    f = os.path.join(run, n, 'scores.json')
    return json.load(open(f)) if os.path.exists(f) else None
hdr = '%-26s %-15s' % ('image', 'linear def/gd7')
for r in runs:
    hdr += ' | %-34s' % (r + ' clip@EV def/gd7/gd8 (>5)')
print(hdr)
for e in plan:
    n = e['name']; L = load('full1', n)['methods']
    ld = min(L['opposed']['all']['dE_mean'], L['segments']['all']['dE_mean'])
    line = '%-26s %5.1f / %5.1f   ' % (n[:26], ld, L['gd-wb4x7']['all']['dE_mean'])
    for r in runs:
        s = load(r, n)
        if not s or 'gd-wb4x8' not in s['methods']:
            line += ' | %-34s' % '-'; continue
        m = s['methods']; a = lambda k: m[k]['all']
        d = min((a('opposed'), a('segments')), key=lambda x: x['dE_mean'])
        line += ' | %3.1f %4.1f/%4.1f/%4.1f (%2.0f/%2.0f/%2.0f%%)' % (
            s['display']['clip_above_grey_ev'], d['dE_mean'], a('gd-wb4x7')['dE_mean'], a('gd-wb4x8')['dE_mean'],
            100 * d['gt5'], 100 * a('gd-wb4x7')['gt5'], 100 * a('gd-wb4x8')['gt5'])
    print(line)
