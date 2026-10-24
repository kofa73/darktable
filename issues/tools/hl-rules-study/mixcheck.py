# mixcheck.py <base workdir>... : checks the composite assumption of the selector simulation
# (Phase 2.4). Renders the clipped frame with the patched darktable (fill-rule.patch,
# GD_FILL_RULE=mix, experiment 7 "hue from measured channels, hue estimate in fully
# clipped"), where each 8-connected region of fully clipped grid pixels gets a fill by a
# hash of its first pixel. Then, with the zones of regions.py, infers per zone which
# single-fill render (7 hue, 8 white, 13 edge) the mixed render matches, builds the
# composite of those renders as analyze.py does, and compares it with the real mixed
# render: pixel differences, and dE2000 per mask exactly as ev2.py scores.
# Writes mixcheck.json into each workdir; the mixed render is deleted.
import sys, os, json
import numpy as np, cv2, colour
from scipy import ndimage
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

ev2.DT = '/tmp/hl-rules-study/dt-build/bin/darktable-cli'
ev2.METHODS['mix7'] = ['6', '--full', '--wb', '4', '--exp', '7']
ev2.METHOD_ENV['mix7'] = {'GD_FILL_RULE': 'mix'}
FILLS = ('gd-wb4x7', 'gd-wb4x8', 'gd-wb4x13')
ZONE = 4


def zones(wd, renders, F):
    """The zone map of regions.py for the 'model' set and the 'cont' set together."""
    H, W = F.shape[:2]
    fullraw = (F >= 0.5).all(2)
    changed = np.zeros((H, W), bool)
    for ms in (('gd-wb4x7', 'gd-wb4x8', 'gd-wb4x13'), ('gd-wb4x10', 'gd-wb4x11', 'gd-wb4x14')):
        h = renders[ms[0]]
        for m in ms[1:]:
            changed |= (np.abs(renders[m] - h) > 1e-3 * np.maximum(np.abs(h), 1e-3)).any(2)
    G = (F > 0).all(2) | fullraw | changed
    nlab, lab = cv2.connectedComponents(G.astype(np.uint8), connectivity=8)
    dist, (iy, ix) = ndimage.distance_transform_edt(~G, return_indices=True)
    owner = lab[iy, ix]
    return np.where(dist <= ZONE, owner, 0), nlab


def main(wd):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    mk = np.load(os.path.join(wd, 'masks.npz'))
    F = mk['F'].astype(np.float32)
    names = list(FILLS) + ['gd-wb4x10', 'gd-wb4x11', 'gd-wb4x14']
    R = {m: np.load(os.path.join(wd, m + '.npy')).astype(np.float32) for m in names}
    zone, nz = zones(wd, R, F)
    synth = sc['k'] if sc['kind'] == 'synth' else None
    size = int(os.environ.get('SYNTH_SIZE', '1600'))
    out = os.path.join('/tmp/hl-rules-study/runs/mixcheck', os.path.basename(wd))
    os.makedirs(out, exist_ok=True)
    mix = ev2.render(sc['clipped'], os.path.join(out, 'mix7.pfm'), 'mix7', ev2.E0 - sc['k'], synth, size)
    if mix.shape != R[FILLS[0]].shape:
        raise SystemExit('shape %s vs %s' % (mix.shape, R[FILLS[0]].shape))
    comp = R[FILLS[0]].copy()
    choice = {}
    zf = zone.ravel()
    order = np.argsort(zf, kind='stable')
    b = np.searchsorted(zf[order], np.arange(nz + 1))
    for r in range(1, nz):
        p = order[b[r]:b[r + 1]]
        if not len(p):
            continue
        d = [float(np.abs(mix.reshape(-1, 3)[p] - R[f].reshape(-1, 3)[p]).sum()) for f in FILLS]
        c = int(np.argmin(d))
        choice[r] = (c, d[c], sorted(d)[1] if len(d) > 1 else 0.0)
        comp.reshape(-1, 3)[p] = R[FILLS[c]].reshape(-1, 3)[p]
    rel = np.abs(comp - mix) / np.maximum(np.abs(mix), 1e-3)
    s = 2.0 ** (sc['k'] - ev2.E0)
    truth = np.load(os.path.join(wd, 'truth.npy')).astype(np.float32)
    if sc['kind'] == 'pair':
        sx, sy = sc['pair_shift'][:2]
        A = np.float32([[1, 0, -sx], [0, 1, -sy]])
        truth = cv2.warpAffine(truth, A, truth.shape[1::-1], flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    t_lab = ev2.lab(ev2.tone(truth, s))
    res = dict(zones=len(choice), choices=[sum(1 for c in choice.values() if c[0] == k) for k in range(3)],
               zones_ambiguous=sum(1 for c in choice.values() if c[2] <= 1.5 * c[1] + 1e-6),
               px_differ=int((rel > 1e-3).any(2).sum()), px_differ_1pct=int((rel > 1e-2).any(2).sum()),
               max_rel=float(rel.max()))
    for m in ('all', 'partial', 'full'):
        v = mk[m]
        if v.sum() < 50:
            continue
        e = lambda img: float(colour.delta_E(t_lab[v], ev2.lab(ev2.tone(img[v], s)), method='CIE 2000').mean())
        res[m] = dict(mix=e(mix), composite=e(comp))
    json.dump(res, open(os.path.join(wd, 'mixcheck.json'), 'w'), indent=1)
    print(os.path.basename(wd), json.dumps(res), flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)
