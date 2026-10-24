# ev3.py: ev2.py with the linear renders of selected methods kept as float16 arrays
# (<method>.npy), for the region table and the selector composites of the rules study.
# EV3_KEEP=<comma list of methods, plus "truth" and "none">; everything else as ev2.py.
# The full PFMs are still deleted (EV2_KEEP_PFM unset): float16 halves the size and
# keeps 3 significant digits, far below the dE resolution the scores report.
import os, sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

KEEP = set(filter(None, os.environ.get('EV3_KEEP', '').split(',')))
_render = ev2.render


def render(raw, out, method, ev, synth, size, tone=None):
    img = _render(raw, out, method, ev, synth, size, tone)
    name = os.path.basename(out)[:-4]
    if name in KEEP and tone is None:
        np.save(out[:-4] + '.npy', img.astype(np.float16))
    return img


ev2.render = render
if __name__ == '__main__':
    ev2.main()
