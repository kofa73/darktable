# renders the Laneway clipped frame (c-def, c-white, c-lrr) and ref 1 (none, as truth) with the
# opposed-controls build, linear Rec2020, 1600 px, as render.py does for the pairs
import sys, os, time
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
sys.path.insert(0, '/tmp/hl-controls/test')
os.environ['EV2_KEEP_PFM'] = '1'
import ev2
ev2.SIDE = '/tmp/hl-controls/test/make_sidecar3.py'
ev2.CFG = '/tmp/hl-controls/cfg'
ev2.DT = '/tmp/hl-controls/build/bin/darktable-cli'
ev2.METHODS.update({
    'c-def': ['5'],
    'c-white': ['5', '--fill-color', '1'],
    'c-lrr': ['5', '--fill-color', '2', '--opp-local', '1', '--opp-passes', '10', '--fill', '4'],
})
D = '/tmp/gradient-domain-eval/iain/'
od = '/tmp/hl-controls/rr/laneway'
os.makedirs(od, exist_ok=True)
jobs = [(D + 'Laneway bracket set clipped.NEF', m) for m in ('c-def', 'c-white', 'c-lrr')]
jobs.append((D + 'Laneway bracket set ref 1.NEF', 'none'))
for raw, m in jobs:
    t = time.time()
    name = 'truth' if m == 'none' else m
    ev2.render(raw, os.path.join(od, name + '.pfm'), m, ev2.E0, None, 1600)
    print(name, '%.1f s' % (time.time() - t), flush=True)
