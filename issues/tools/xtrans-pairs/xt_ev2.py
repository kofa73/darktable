# xt_ev2.py <reference raw> <clipped raw> <dEV> <workdir> [methods...]: ev2.py pair with the
# opposed-controls build, with the settings names of render.py (2026-10-27-baseline), for the
# X-Trans pairs. Writes truth.pfm, <method>.pfm, masks.npz, scores.json into the workdir.
import sys, os
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
os.environ['EV2_KEEP_PFM'] = '1'
import ev2
ev2.SIDE = '/tmp/hl-controls/test/make_sidecar3.py'
ev2.DT = '/tmp/hl-controls/build/bin/darktable-cli'
ev2.CFG = os.environ['SYNTH_CFG']
ev2.METHODS.update({
    # from rim, no fill, specks 0 (for the comparison without per region)
    'c-p11r': ['5', '--opp-passes', '11', '--fill-color', '2', '--fill-specks', '0'],
    # Kofa's g065 export: per region, from rim, flat lift (max rise 2), specks 64, reference passes 0
    'c-k065': ['5', '--opp-local', '1', '--fill-color', '2', '--fill', '1'],
    # the candidate: the same with reference passes 11 (warm lights)
    'c-k065p': ['5', '--opp-passes', '11', '--opp-local', '1', '--fill-color', '2', '--fill', '1'],
})
sys.argv = ['ev2.py', 'pair'] + sys.argv[1:]
ev2.main()
