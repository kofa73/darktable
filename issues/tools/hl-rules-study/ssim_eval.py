# ssim_eval.py <workdir>... : SSIMULACRA 2 (rust-av/ssimulacra2, built in /tmp/ssimulacra2-src) of
# each method's display render (<method>_d.png from ev2's EV2_DISPLAY mode) against the truth's
# display render, on a composite: the method's pixels within DIL px of the clipped pixels (raw flags
# F > 0 in any channel, or ev2's 'all' mask), the truth's pixels elsewhere. This keeps residual
# misalignment, noise and exposure differences of real pairs out of the score, so that only the
# reconstruction differs; the score of a method is therefore comparable only to other methods on
# the same image. Higher is better (100 = identical). Also the worst 256 px tile (half-tile steps)
# among tiles with >= 10% of pixels in that region, and ev2's display dE (mean, 'all').
import sys, os, json, subprocess, tempfile
import numpy as np, cv2

BIN = '/tmp/ssimulacra2-src/ssimulacra2/target/release/ssimulacra2_rs'
DIL = int(os.environ.get('SSIM_DIL', '25'))


def score(a, b):
    with tempfile.TemporaryDirectory() as d:
        pa, pb = os.path.join(d, 'a.png'), os.path.join(d, 'b.png')
        cv2.imwrite(pa, a); cv2.imwrite(pb, b)
        out = subprocess.run([BIN, 'image', '--no-icc', pa, pb], capture_output=True, text=True, check=True).stdout
    return float(out.split(':')[1])


def main(wd, methods):
    mk = np.load(os.path.join(wd, 'masks.npz'))
    clip = (mk['F'].astype(np.float32) > 0).any(2) | mk['all']
    region = cv2.dilate(clip.astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * DIL + 1,) * 2)) > 0
    truth = cv2.imread(os.path.join(wd, 'truth_d.png'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    res = {}
    for m in methods:
        img = cv2.imread(os.path.join(wd, m + '_d.png'))
        comp = truth.copy(); comp[region] = img[region]
        # worst 256 px tile with at least 10% of its pixels near clipped ones: a local blotch
        # counts fully instead of being averaged over the image
        T = 256
        tiles = []
        for y in range(0, truth.shape[0] - T + 1, T // 2):
            for x in range(0, truth.shape[1] - T + 1, T // 2):
                if region[y:y + T, x:x + T].mean() >= 0.1:
                    tiles.append(score(truth[y:y + T, x:x + T], comp[y:y + T, x:x + T]))
        res[m] = dict(ssimulacra2=score(truth, comp), worst_tile=min(tiles) if tiles else None,
                      dE=sc['methods'][m]['all']['dE_mean'])
    res['region_fraction'] = float(region.mean())
    json.dump(res, open(os.path.join(wd, 'ssim.json'), 'w'), indent=1)
    print('%-26s region %4.1f%%  ' % (os.path.basename(wd.rstrip('/')), 100 * res['region_fraction'])
          + '  '.join('%s ssim2 %6.2f worst tile %6.2f dE %5.2f' % (m, res[m]['ssimulacra2'], res[m]['worst_tile'] or float('nan'),
                                                                 res[m]['dE']) for m in methods))


if __name__ == '__main__':
    ms = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
    for wd in sys.argv[1:]:
        main(wd, ms)
