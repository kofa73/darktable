# crop_sheet.py <out.png> <pair workdir>...: crops of the clipped areas that no frame records, for a
# visual check. Needs workdirs that pair_align.py has processed (align/ with the aligned truth and the
# truth's own clipping warped by nona) and the linear renders kept with EV2_KEEP_PFM.
# Per workdir: pixels clipped in the clipped frame (masks.npz F) and also in the truth frame (warped
# raw flags > 0.02, as pair_align.py's noTruth) are grouped into regions (closed with a 7 px disk,
# so the pieces of one light merge); the CROPS largest of at least MIN_PX pixels get one row each:
# inpaint opposed and gradient domain at 0 EV and -2 EV, and the aligned truth frame at -2 EV.
# Linear output as pair_align.py and lin_eval.py: clipping level of the clipped frame at display
# white, no tone curve. The truth panel is clipped in the region too; it only shows the context.
# A green outline marks the region in the truth panel, where unrecorded pixels are flat gray.
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev2
from lin_eval import to_srgb8 as to_srgb8_clip

# CS_HUEKEEP=1 (issue 29): a pixel over display white is scaled down as a whole by its largest sRGB
# channel instead of clipping each channel by itself, so that the panels show hue and brightness
# gradients up to white, not the hue shifts of per-channel clipping (issue 28)
HUEKEEP = os.environ.get('CS_HUEKEEP') == '1'


def to_srgb8(lin):
    if not HUEKEEP:
        return to_srgb8_clip(lin)
    a = np.maximum(np.maximum(lin, 0) @ ev2.TO_SRGB.T, 0)
    a = a / np.maximum(a.max(-1, keepdims=True), 1)
    s = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    return (s * 255 + 0.5).astype(np.uint8)

CROPS = int(os.environ.get('CS_CROPS', '3'))
MIN_PX = int(os.environ.get('CS_MIN_PX', '40'))
H = int(os.environ.get('CS_HEIGHT', '200'))
# exposures of the method panels, comma list in EV (the truth panel stays at -2 EV)
EVS = [int(v) for v in os.environ.get('CS_EVS', '0,-2').split(',')]
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
SELECT = os.environ.get('CS_SELECT', 'unrecorded')
# panel labels: short forms of the GUI names ("continuous border, ... in fully clipped")
LABEL = {'opposed': 'inpaint opposed', 'gd-wb4x10': 'CB hue fill', 'gd-wb4x11': 'CB white fill',
         'gd-wb4x13': 'measured, edge fill', 'gd-wb4x14': 'CB edge fill', 'gd-dome': 'dome (log)',
         'gd-domelin': 'dome (linear)',
         # reduced resolution ("full resolution" off)
         'gd-r-hold': 'CB hue fill', 'gd-r-wb11': 'CB white fill', 'gd-r-wb14': 'CB edge fill',
         'gd-r-nohold': 'no hold, joint capped', 'gd-r-nohold-wb0': 'no hold, basic',
         'gd-r-dome': 'dome (log)', 'gd-r-domelin': 'dome (linear)',
         # issue 29: inpaint opposed with a brightness fill of the fully clipped regions; "small":
         # regions up to 50,000 render pixels only
         'opp-flat': 'opp. flat lift', 'opp-domelin': 'opp. dome lin', 'opp-domelog': 'opp. dome log',
         'opp-domelin-s': 'opp. dome lin small', 'opp-domelog-s': 'opp. dome log small',
         # issue 31 (2026-10-27): opposed-controls build; '|' starts a new line, the legend
         # (CS_LEGEND) gives the full settings
         'c-def': 'inpaint opposed|defaults', 'c-p11': 'passes 11',
         'c-p11l': 'passes 11|per region', 'c-p11cl': 'passes 11|per region|cube root',
         'c-p11r': 'passes 11|from rim', 'c-nodome': 'Laneway|no dome', 'c-best': 'Laneway|with dome'}
# CS_LEGEND: lines (separated by '|') printed above the first row, to explain the short labels
LEGEND = [l for l in os.environ.get('CS_LEGEND', '').split('|') if l]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)


def text(img, s, y, scale=0.45):
    # multi-line labels ('|'), the last line at y
    lines = s.split('|')
    if len(lines) > 1:
        for k, l in enumerate(lines):
            text(img, l, y - 15 * (len(lines) - 1 - k), scale)
        return
    cv2.putText(img, s, (4, y), cv2.FONT_HERSHEY_SIMPLEX, scale, (0, 0, 0), 3, cv2.LINE_AA)
    cv2.putText(img, s, (4, y), cv2.FONT_HERSHEY_SIMPLEX, scale, (255, 255, 255), 1, cv2.LINE_AA)


def rows(wd):
    name = os.path.basename(wd.rstrip('/'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    s = 2.0 ** (sc['k'] - ev2.E0)
    al = os.path.join(wd, 'align')
    ta = cv2.imread(os.path.join(al, 'al_0001.tif'), cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]),
                    cv2.IMREAD_UNCHANGED)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5:
        Ra = Ra / np.iinfo(mk.dtype).max
    F = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32)
    clip = F.max(2) > 0
    unrec = clip & (Ra > 0.02)
    # CS_SELECT=full (issue 29): crop the largest fully clipped regions, recorded by the truth frame
    # or not, and show the truth panel at every exposure of the method panels
    core = (F.min(2) >= 0.5) if SELECT == 'full' else unrec
    n, lab, st, _ = cv2.connectedComponentsWithStats(cv2.morphologyEx(core.astype(np.uint8), cv2.MORPH_CLOSE, disk(3)), 8)
    # area of the unrecorded pixels themselves, not of the closed mask
    area = np.bincount(lab[core], minlength=n)
    order = [i for i in np.argsort(-area) if i and area[i] >= MIN_PX][:CROPS]
    if not order:
        return [], '%s: no unrecorded clipped region of %d px or more (%d px in all)' % (name, MIN_PX, core.sum())
    imgs = {m: ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) * s for m in METHODS}
    out = []
    for i in order:
        x, y, w, h = st[i, :4]
        # margin: the region's own size, at least 30 px, so the surroundings show
        mg = max(30, w, h)
        x0, y0 = max(0, x - mg), max(0, y - mg)
        x1, y1 = min(clip.shape[1], x + w + mg), min(clip.shape[0], y + h + mg)
        sl = np.s_[y0:y1, x0:x1]
        # panels H px high and at most 1.8 H wide; wide crops are padded with gray below
        f = min(H / (y1 - y0), 1.8 * H / (x1 - x0))
        size = (max(1, round((x1 - x0) * f)), max(1, round((y1 - y0) * f)))
        panels = []
        for off in EVS:
            for m in METHODS:
                p = cv2.resize(to_srgb8(imgs[m][sl] * 2.0 ** off), size, interpolation=cv2.INTER_AREA if f < 1 else cv2.INTER_NEAREST)
                panels.append((p, '%s|%+d EV' % (LABEL.get(m, m), off)))
        cs, _ = cv2.findContours((lab[sl] == i).astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        for off in (EVS if SELECT == 'full' else [-2]):
            t = to_srgb8(T[sl] * 2.0 ** off)
            # the truth's clipped pixels would show the raw clipping's magenta: no data, so flat gray
            t[unrec[sl]] = 128
            cv2.drawContours(t, cs, -1, (0, 255, 0), 1)
            panels.append((cv2.resize(t, size, interpolation=cv2.INTER_AREA if f < 1 else cv2.INTER_NEAREST), 'truth frame|%+d EV' % off))
        # an extra panel with the region tinted green: an outline alone that runs along the crop
        # border reads as a small circled area (g065, 2026-10-27); the truth panels keep their colors
        t = to_srgb8(T[sl] * 2.0 ** -2)
        t[unrec[sl]] = 128
        reg = lab[sl] == i
        t[reg] = (0.6 * t[reg] + 0.4 * np.array([0, 255, 0])).astype(np.uint8)
        panels.append((cv2.resize(t, size, interpolation=cv2.INTER_AREA if f < 1 else cv2.INTER_NEAREST),
                       'clipped region|(green)'))
        row = []
        for p, lbl in panels:
            p = np.concatenate([p, np.full((H - p.shape[0], p.shape[1], 3), 128, np.uint8)])
            text(p, lbl, H - 6)
            row += [p, np.full((H, 4, 3), 128, np.uint8)]
        row = np.concatenate(row, 1)
        bar = np.full((22, row.shape[1], 3), 255, np.uint8)
        cv2.putText(bar, '%s  region %d px at (%d, %d)' % (name, area[i], x, y), (4, 16), cv2.FONT_HERSHEY_SIMPLEX,
                    0.5, (0, 0, 0), 1, cv2.LINE_AA)
        out.append(np.concatenate([bar, row]))
    return out, '%s: %d regions, largest %s px' % (name, len(order), ', '.join(str(area[i]) for i in order))


def main():
    allrows = []
    for wd in sys.argv[2:]:
        r, msg = rows(wd)
        print(msg, flush=True)
        allrows += r
    if not allrows:
        return
    W = max(r.shape[1] for r in allrows)
    if LEGEND:
        bar = np.full((10 + 20 * len(LEGEND), W, 3), 255, np.uint8)
        for k, l in enumerate(LEGEND):
            cv2.putText(bar, l, (4, 20 + 20 * k), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 1, cv2.LINE_AA)
        allrows = [bar] + allrows
    pad = lambda r: np.concatenate([r, np.full((r.shape[0], W - r.shape[1], 3), 255, np.uint8)], 1)
    sheet = np.concatenate([np.concatenate([pad(r), np.full((6, W, 3), 255, np.uint8)]) for r in allrows])
    cv2.imwrite(sys.argv[1], cv2.cvtColor(sheet, cv2.COLOR_RGB2BGR))


if __name__ == '__main__':
    main()
