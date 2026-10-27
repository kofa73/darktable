# render.py <workdir>...: renders the methods in R_METHODS (comma list) of the clipped frame of each
# ev2 pair workdir with the opposed-controls build, at the size and exposure of the scored renders,
# into the workdir as <method>.pfm (skipped if present), so pair_align.py can score them.
# Config directory per worker: R_CFG.
import sys, os, json
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
os.environ['EV2_KEEP_PFM'] = '1'
import ev2
ev2.SIDE = '/tmp/hl-controls/test/make_sidecar3.py'
ev2.CFG = os.environ['R_CFG']
ev2.DT = '/tmp/hl-controls/build/bin/darktable-cli'
# Laneway best settings of Kofa's GUI test, export _17 (issue 31)
BEST = ['5', '--opp-passes', '11', '--opp-iterate', '0', '--opp-cbrt', '1', '--opp-local', '1',
        '--fill-color', '2', '--fill-specks', '0']
ev2.METHODS.update({
    'c-def': ['5'],
    'c-best': BEST + ['--fill', '4', '--fill-strength', '4', '--fill-max-rise', '4'],
    'c-nodome': BEST,
    # ablation of c-nodome for the sky tint: one control added at a time
    'c-p11': ['5', '--opp-passes', '11', '--fill-specks', '0'],
    'c-p11l': ['5', '--opp-passes', '11', '--opp-local', '1', '--fill-specks', '0'],
    'c-p11cl': ['5', '--opp-passes', '11', '--opp-cbrt', '1', '--opp-local', '1', '--fill-specks', '0'],
    'c-p11r': ['5', '--opp-passes', '11', '--fill-color', '2', '--fill-specks', '0'],
    # Kofa 2026-10-27: "from rim" without the cube root is good on skies; score it on the 103 pairs
    'c-p11lr': ['5', '--opp-passes', '11', '--opp-local', '1', '--fill-color', '2', '--fill-specks', '0'],
    'c-p11r-d1': ['5', '--opp-passes', '11', '--fill-color', '2', '--fill-specks', '0', '--fill', '4',
                  '--fill-strength', '1'],
    # Kofa's g065 export (2026-10-27 19:56): per region, from rim, flat lift (max rise 2), specks 64,
    # reference passes 0; and the same with reference passes 11 (warm lights)
    'c-k065': ['5', '--opp-local', '1', '--fill-color', '2', '--fill', '1'],
    'c-k065p': ['5', '--opp-passes', '11', '--opp-local', '1', '--fill-color', '2', '--fill', '1'],
    'c-p11r-d2': ['5', '--opp-passes', '11', '--fill-color', '2', '--fill-specks', '0', '--fill', '4',
                  '--fill-strength', '2', '--fill-max-rise', '4'],
})
for wd in sys.argv[1:]:
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    for m in os.environ['R_METHODS'].split(','):
        out = os.path.join(wd, m + '.pfm')
        if os.path.exists(out):
            continue
        ev2.render(sc['clipped'], out, m, ev2.E0 - sc['k'], None, 1600)
    print('done', os.path.basename(wd.rstrip('/')),
          ' '.join('%s %.1fs' % (k[:-4], v) for k, v in ev2.TIMES.items()), flush=True)
    ev2.TIMES.clear()
