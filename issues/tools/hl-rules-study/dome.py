# dome.py <workdir>...: issue dome-for-clipped-cores, offline prototype. Writes <DOME_OUT>.pfm next to
# the method's linear render <DOME_SRC>.pfm (default gd-wb4x10 -> gd-dome), so that pair_align.py and
# crop_sheet.py can treat it as one more method.
# Gradient domain fills fully clipped regions with a Laplace solve, so a core can never be brighter
# than its rim (maximum principle). Here, per fully clipped region (8-connected; a render pixel counts
# when all three channels have at least half of their photosites clipped, masks.npz F, as
# regions.py), log luminance is refilled with a biharmonic extrapolation: the squared Laplacian is
# minimized over the region with everything outside it fixed, on a crop of the region plus a margin
# of its own size (at least 8 px). That continues rising slopes into a dome. Where the dome is
# brighter, the pixel's RGB is scaled up (hue kept), at most DOME_MAX_EV; it never darkens a pixel.
# Regions above DOME_MAX_PX render pixels (skies) are left unchanged.
import sys, os, json
import numpy as np, cv2
import scipy.sparse as sp
import scipy.sparse.linalg as spl
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

SRC = os.environ.get('DOME_SRC', 'gd-wb4x10')
OUT = os.environ.get('DOME_OUT', 'gd-dome')
MAX_PX = int(os.environ.get('DOME_MAX_PX', '50000'))
MAX_EV = float(os.environ.get('DOME_MAX_EV', '4'))
# DOME_LINEAR=1: the solve on linear luminance instead of its log, where a lamp's glow is shallower
LINEAR = os.environ.get('DOME_LINEAR') == '1'


def write_pfm(path, a):
    # inverse of pfm.read_pfm: little endian, rows stored bottom to top
    h, w = a.shape[:2]
    with open(path, 'wb') as f:
        f.write(b'PF\n%d %d\n-1.0\n' % (w, h))
        np.ascontiguousarray(a[::-1], '<f4').tofile(f)


def laplacian(h, w):
    # 5-point Laplacian on the crop; at the crop edge only the neighbors inside count
    n = h * w
    idx = np.arange(n).reshape(h, w)
    rows, cols, vals = [], [], []
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        src = idx[max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)].ravel()
        dst = idx[max(0, dy):h - max(0, -dy) or None, max(0, dx):w - max(0, -dx) or None].ravel()
        rows += [src, src]
        cols += [dst, src]
        vals += [np.ones(src.size), -np.ones(src.size)]
    return sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(n, n))


def dome(img, full):
    out = img.copy()
    Y = np.maximum(ev2.lum(np.maximum(img, 0)), 1e-6)
    L = Y if LINEAR else np.log(Y)
    n, lab, st, _ = cv2.connectedComponentsWithStats(full.astype(np.uint8), 8)
    H, W = full.shape
    done = skipped = 0
    for i in range(1, n):
        x, y, w, h, area = st[i]
        if area > MAX_PX:
            skipped += 1
            continue
        m = max(8, w, h)
        y0, y1, x0, x1 = max(0, y - m), min(H, y + h + m), max(0, x - m), min(W, x + w + m)
        reg = lab[y0:y1, x0:x1] == i
        ch, cw = reg.shape
        D = laplacian(ch, cw)
        u = np.flatnonzero(reg.ravel())
        k = np.flatnonzero(~reg.ravel())
        Du, Dk = D[:, u], D[:, k]
        A = (Du.T @ Du).tocsc()
        b = -(Du.T @ (Dk @ L[y0:y1, x0:x1].ravel()[k]))
        sol = spl.spsolve(A, b)
        cur = L[y0:y1, x0:x1].ravel()[u]
        rise = np.log(np.maximum(sol, 1e-6) / cur) if LINEAR else sol - cur
        rise = np.clip(rise, 0, MAX_EV * np.log(2))
        g = np.ones(ch * cw)
        g[u] = np.exp(rise)
        out[y0:y1, x0:x1] *= g.reshape(ch, cw)[..., None]
        done += 1
    return out, done, skipped


def main(wd):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    img = ev2.read_pfm(os.path.join(wd, SRC + '.pfm')).astype(np.float32)
    F = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32)
    full = F.min(2) >= 0.5
    out, done, skipped = dome(img, full)
    write_pfm(os.path.join(wd, OUT + '.pfm'), out.astype(np.float32))
    s = 2.0 ** (sc['k'] - ev2.E0)
    rose = (out.max(2) > img.max(2) * 1.01)
    print('%-44s %d regions domed, %d skipped (> %d px); %.2f%% of pixels raised, median x%.2f there' % (
        os.path.basename(wd.rstrip('/'))[:44], done, skipped, MAX_PX, 100 * rose.mean(),
        float(np.median(ev2.lum(out[rose]) / np.maximum(ev2.lum(img[rose]), 1e-6))) if rose.any() else 1.0), flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)
