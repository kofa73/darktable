# render_extra.py <workdir>...: renders the methods in RX_METHODS (comma list, ev4.py names) of the
# clipped frame into existing ev2 pair workdirs, with the exposure and size ev2.py used
# (ev_m = E0 - k, SYNTH_SIZE), keeping the PFM, so that pair_align.py (SSIM_METHODS) can score them
# next to the methods of the original run. Skips methods whose PFM exists. Uses SYNTH_CFG.
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ['EV2_KEEP_PFM'] = '1'
import ev4
ev2 = ev4.ev2

size = int(os.environ.get('SYNTH_SIZE', '1600'))
for wd in sys.argv[1:]:
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    for m in os.environ['RX_METHODS'].split(','):
        out = os.path.join(wd, m + '.pfm')
        if os.path.exists(out):
            continue
        ev2.render(sc['clipped'], out, m, ev2.E0 - sc['k'], None, size)
    # wall time per render (ev2.TIMES), for the 1 minute limit of issue solver-accuracy-and-speed
    print('done', os.path.basename(wd.rstrip('/')),
          ' '.join('%s %.0fs' % (k[:-4], v) for k, v in ev2.TIMES.items() if k.startswith(tuple(os.environ['RX_METHODS'].split(',')))),
          flush=True)
    ev2.TIMES.clear()
