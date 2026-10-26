# ev4.py: ev2.py with the darktable of /tmp/hl-rules-study/dt-build (experiment switches of
# patches/hanno-variants.patch) and make_sidecar2.py (rebuild mode and strength), plus the
# methods of the inpaint opposed / segmentation based study. Switches unset = HEAD code.
import os, sys
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

ev2.DT = '/tmp/hl-rules-study/dt-build/bin/darktable-cli'
ev2.SIDE = '/tmp/hl-rules-study/scripts/make_sidecar2.py'
RB = ['4', '--recovery', '5', '--strength', '0.5']   # rebuild "generic", strength 0.5
for name, args, env in (
        ('opp-cons', ['5'], {'DT_OPP_CONSISTENT': '1'}),
        ('opp-asshot', ['5'], {'DT_OPP_SCALE': 'asshot'}),
        ('seg-thresh', ['4'], {'DT_SEG_FIX': 'thresh'}),
        ('seg-full', ['4'], {'DT_SEG_FIX': 'full', 'DT_OPP_CONSISTENT': '1'}),
        ('seg-asshot', ['4'], {'DT_SEG_FIX': 'asshot', 'DT_OPP_SCALE': 'asshot'}),
        ('segrb', RB, {}),
        ('segrb-thresh', RB, {'DT_SEG_FIX': 'thresh'}),
        ('segrb-thresh-cbrt', RB, {'DT_SEG_FIX': 'thresh', 'DT_SEG_PLANE': 'cbrt'}),
        ('segrb-full-cbrt', RB, {'DT_SEG_FIX': 'full', 'DT_SEG_PLANE': 'cbrt', 'DT_OPP_CONSISTENT': '1'}),
        ('segrb-asshot-cbrt', RB, {'DT_SEG_FIX': 'asshot', 'DT_SEG_PLANE': 'cbrt', 'DT_OPP_SCALE': 'asshot'})):
    ev2.METHODS[name] = args
    ev2.METHOD_ENV[name] = env
# segmentation based slider sweep, without and with the threshold fix: seg-c<combine>-k<candidating>[-t]
for comb in (1, 2, 4):
    for cand in (0.2, 0.5, 0.8):
        name = 'seg-c%d-k%d' % (comb, round(cand * 10))
        ev2.METHODS[name] = ev2.METHODS[name + '-t'] = ['4', '--combine', str(comb), '--candidating', str(cand)]
        ev2.METHOD_ENV[name] = {}
        ev2.METHOD_ENV[name + '-t'] = {'DT_SEG_FIX': 'thresh'}
# border classification (issue border-classification): continuous border plus the occluder test of
# DT_GD_OCCLUDER in gradient.c's _gd_hue_domain(), threshold t of the largest channel within 5 px
for t in (0.15, 0.3):
    name = 'gd-occ%d' % round(t * 100)
    ev2.METHODS[name] = ev2.METHODS['gd-wb4x10']
    ev2.METHOD_ENV[name] = {'DT_GD_OCCLUDER': str(t)}
# partly clipped tint (issue partly-clipped-sky-tint): continuous border, with opposed's estimate as
# the hue of pixels with two flagged channels (DT_GD_OPP_TWO in gradient.c)
ev2.METHODS['gd-opp2'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-opp2'] = {'DT_GD_OPP_TWO': '1'}
# the same with one opposed offset per clipped region (DT_GD_OPP_LOCAL), and "opposed's estimate as
# hue, hue estimate in fully clipped" with it
ev2.METHODS['gd-opp2l'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-opp2l'] = {'DT_GD_OPP_TWO': '1', 'DT_GD_OPP_LOCAL': '1'}
ev2.METHODS['gd-x9l'] = ev2.METHODS['gd-wb4x9']
ev2.METHOD_ENV['gd-x9l'] = {'DT_GD_OPP_LOCAL': '1'}
# holding (issue holding-and-writeback): "continuous border, hue estimate in fully clipped" without
# holding at the input (DT_GD_NO_HOLD), with write-back "joint, any channel clipped, capped" (4) and
# "basic" (0)
ev2.METHODS['gd-nohold'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-nohold'] = {'DT_GD_NO_HOLD': '1'}
ev2.METHODS['gd-nohold-wb0'] = ['6', '--full', '--wb', '0', '--exp', '10']
ev2.METHOD_ENV['gd-nohold-wb0'] = {'DT_GD_NO_HOLD': '1'}
# the same at reduced resolution (full resolution off), with and without holding
ev2.METHODS['gd-r-hold'] = ['6', '--wb', '4', '--exp', '10']
ev2.METHOD_ENV['gd-r-hold'] = {}
ev2.METHODS['gd-r-nohold'] = ['6', '--wb', '4', '--exp', '10']
ev2.METHOD_ENV['gd-r-nohold'] = {'DT_GD_NO_HOLD': '1'}
# hue bound (issue hue-estimate-channel-bound, DT_GD_HUE_BOUND): current code with it, without holding,
# and without holding at reduced resolution
ev2.METHODS['gd-bound'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-bound'] = {'DT_GD_HUE_BOUND': '1'}
ev2.METHODS['gd-bound-nohold'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-bound-nohold'] = {'DT_GD_HUE_BOUND': '1', 'DT_GD_NO_HOLD': '1'}
ev2.METHODS['gd-r-bound-nohold'] = ['6', '--wb', '4', '--exp', '10']
ev2.METHOD_ENV['gd-r-bound-nohold'] = {'DT_GD_HUE_BOUND': '1', 'DT_GD_NO_HOLD': '1'}
# inpaint opposed with one chroma offset per clipped region (issue opposed-local-chroma, DT_OPP_LOCAL)
ev2.METHODS['opp-local'] = ['5']
ev2.METHOD_ENV['opp-local'] = {'DT_OPP_LOCAL': '1'}
# fringe test (issue fringe-color-sources, DT_GD_FRINGE): continuous border, chromaticity distance t
for t in (0.05, 0.1):
    name = 'gd-fringe%02d' % round(t * 100)
    ev2.METHODS[name] = ev2.METHODS['gd-wb4x10']
    ev2.METHOD_ENV[name] = {'DT_GD_FRINGE': str(t)}
# solver speed (issue solver-accuracy-and-speed): hue estimate by the convolution pyramid membrane
# (DT_GD_HUE_PYR), at full and at reduced resolution
ev2.METHODS['gd-pyr'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-pyr'] = {'DT_GD_HUE_PYR': '1'}
ev2.METHODS['gd-r-pyr'] = ['6', '--wb', '4', '--exp', '10']
ev2.METHOD_ENV['gd-r-pyr'] = {'DT_GD_HUE_PYR': '1'}
# hue estimate by the smooth pull-push fill alone, no Laplace solves (DT_GD_HUE_PP)
ev2.METHODS['gd-pp'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-pp'] = {'DT_GD_HUE_PP': '1'}
ev2.METHODS['gd-r-pp'] = ['6', '--wb', '4', '--exp', '10']
ev2.METHOD_ENV['gd-r-pp'] = {'DT_GD_HUE_PP': '1'}
# CG iteration cap at reduced resolution (DT_GD_CG_MAXITER): does stopping early help?
for it in (50, 200, 1000):
    ev2.METHODS['gd-r-it%d' % it] = ['6', '--wb', '4', '--exp', '10']
    ev2.METHOD_ENV['gd-r-it%d' % it] = {'DT_GD_CG_MAXITER': str(it)}
# reduced-resolution redo of issues holding-and-writeback and rescore-fills-and-selectors
# (HANDOFF 2026-10-24 step 1): no holding with write-back "basic", white fill, edge fill
ev2.METHODS['gd-r-nohold-wb0'] = ['6', '--wb', '0', '--exp', '10']
ev2.METHOD_ENV['gd-r-nohold-wb0'] = {'DT_GD_NO_HOLD': '1'}
ev2.METHODS['gd-r-wb11'] = ['6', '--wb', '4', '--exp', '11']
ev2.METHOD_ENV['gd-r-wb11'] = {}
ev2.METHODS['gd-r-wb14'] = ['6', '--wb', '4', '--exp', '14']
ev2.METHOD_ENV['gd-r-wb14'] = {}
# the round-4 multigrid solver ported into dt-src (DT_GD_MG, issue solver-accuracy-and-speed item 11),
# reduced resolution, to compare with Sol's own renders gd-r-mg3
ev2.METHODS['gd-r-mg4'] = ['6', '--wb', '4', '--exp', '10']
ev2.METHOD_ENV['gd-r-mg4'] = {'DT_GD_MG': '1'}
# full resolution with the multigrid solver (issue solver-accuracy-and-speed item 12)
ev2.METHODS['gd-mg4f'] = ev2.METHODS['gd-wb4x10']
ev2.METHOD_ENV['gd-mg4f'] = {'DT_GD_MG': '1'}
# issue opposed-hue-blend: hue of partly clipped pixels blended with opposed's, gd^t * opp^(1-t)
for t in (25, 50):
    ev2.METHODS['gd-r-hb%d' % t] = ['6', '--wb', '4', '--exp', '10']
    ev2.METHOD_ENV['gd-r-hb%d' % t] = {'DT_GD_MG': '1', 'DT_GD_HUE_BLEND': '%.2f' % (t / 100)}
# issue opposed-hue-blend: output blend opp^(1-t) * gd^t per photosite (DT_HL_BLEND, highlights.c)
for t in (25,):
    ev2.METHODS['gd-r-ob%d' % t] = ['6', '--wb', '4', '--exp', '10']
    ev2.METHOD_ENV['gd-r-ob%d' % t] = {'DT_GD_MG': '1', 'DT_HL_BLEND': '%.2f' % (t / 100)}
# issue opposed-two-clipped-ring (30): inpaint opposed from the rebuilt dt-src (upstream master
# e1e2e26baa plus the switches), baseline and iterated references (DT_OPP_ITER), offsets in
# cube-root space (DT_OPP_CBRT)
for name, env in (('opp-b', {}), ('opp-it1', {'DT_OPP_ITER': '1'}), ('opp-it10', {'DT_OPP_ITER': '10'}),
                  ('opp-cit10', {'DT_OPP_ITER': '10', 'DT_OPP_CBRT': '1'}), ('opp-c', {'DT_OPP_CBRT': '1'}),
                  # iteration only for red and green with blue measured (DT_OPP_ITER_RG)
                  ('opp-rg10', {'DT_OPP_ITER': '10', 'DT_OPP_ITER_RG': '1'}),
                  ('opp-rg2', {'DT_OPP_ITER': '2', 'DT_OPP_ITER_RG': '1'})):
    ev2.METHODS[name] = ['5']
    ev2.METHOD_ENV[name] = env
if __name__ == '__main__':
    ev2.main()
