# pair_table.py <workdir>...: table of pair_align.py results, per pair and per scene type (the bracket
# directory: city, colored, indoor, night, phone, sky, specular).
# Per pair and exposure (0 and -1 EV, or PT_EVS): d = SSIMULACRA 2 of gradient domain minus inpaint opposed;
# |d| < TIE is a tie, otherwise a win for the higher score. A pair counts as a gradient domain win
# if it wins at one exposure and does not lose at the other; as a loss in the mirror case; as
# mixed if it wins at one and loses at the other; else as a tie.
# Excluded from the counts (listed with the reason): motion (pair_align.py, blurred) above MOTION,
# a truth for less than SHARE of the clipped pixels, or a missing pair_align.json (PT_IN: other name).
import sys, os, json
from collections import defaultdict

TIE = float(os.environ.get('PT_TIE', '2'))
MOTION = float(os.environ.get('PT_MOTION', '0.15'))
SHARE = float(os.environ.get('PT_SHARE', '0.5'))
A, B = os.environ.get('PT_METHODS', 'opposed,gd-wb4x10').split(',')
# exposures, in EV, as in pair_align.py's PA_EVS
EVS = [int(x) for x in os.environ.get('PT_EVS', '0,-1').split(',')]


def verdict(d):
    return 0 if abs(d) < TIE else (1 if d > 0 else -1)


rows, counts, excl = [], defaultdict(lambda: defaultdict(int)), []
for wd in sys.argv[1:]:
    name = os.path.basename(wd.rstrip('/'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    kind = sc['ref'].split('/bracketed/')[1].split('/')[0] if '/bracketed/' in sc['ref'] else '?'
    p = os.path.join(wd, os.environ.get('PT_IN', 'pair_align.json'))
    if not os.path.exists(p):
        excl.append((kind, name, 'no pair_align.json (alignment failed?)'))
        continue
    r = json.load(open(p))
    if r['motion'] is not None and r['motion'] > MOTION:
        excl.append((kind, name, 'motion %.0f%%' % (100 * r['motion'])))
        continue
    if r['truth_share'] < SHARE:
        excl.append((kind, name, 'truth for %.0f%% of clipped px' % (100 * r['truth_share'])))
        continue
    d = [r['%s %+d' % (B, o)] - r['%s %+d' % (A, o)] for o in EVS]
    v = [verdict(x) for x in d]
    tot = 'mixed' if 1 in v and -1 in v else 'win' if 1 in v else 'loss' if -1 in v else 'tie'
    counts[kind][tot] += 1
    rows.append((kind, name, r, d, tot))

print('gradient domain (%s) against inpaint opposed (%s), SSIMULACRA 2, tie if |d| < %g' % (B, A, TIE))
print('%-9s %-42s %5s %5s  ' % ('type', 'pair', 'truth', 'motn')
      + '  '.join('%6s %6s %6s' % ('opp%+d' % o, 'gd%+d' % o, 'd%+d' % o) for o in EVS) + '  verdict')
for kind, name, r, d, tot in sorted(rows):
    print('%-9s %-42s %4.0f%% %4.1f%%  ' % (kind, name[:42], 100 * r['truth_share'], 100 * (r['motion'] or 0))
          + '  '.join('%6.1f %6.1f %+6.1f' % (r['%s %+d' % (A, o)], r['%s %+d' % (B, o)], x) for o, x in zip(EVS, d))
          + '  ' + tot)
print()
print('%-9s %4s %4s %4s %5s %5s' % ('type', 'win', 'loss', 'tie', 'mixed', 'excl'))
ex = defaultdict(int)
for kind, _, _ in excl:
    ex[kind] += 1
for kind in sorted(set(counts) | set(ex)):
    c = counts[kind]
    print('%-9s %4d %4d %4d %5d %5d' % (kind, c['win'], c['loss'], c['tie'], c['mixed'], ex[kind]))
c = defaultdict(int)
for k in counts.values():
    for t, n in k.items():
        c[t] += n
print('%-9s %4d %4d %4d %5d %5d' % ('all', c['win'], c['loss'], c['tie'], c['mixed'], len(excl)))
print()
print('excluded (motion > %.0f%%, truth < %.0f%%):' % (100 * MOTION, 100 * SHARE))
for kind, name, why in sorted(excl):
    print('  %-9s %-42s %s' % (kind, name, why))
