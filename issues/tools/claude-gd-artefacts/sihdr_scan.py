# sihdr_scan.py <raw dir> <out.json> [workers] [extension]
# Finds real clipped/reference pairs in bracketed tripod data sets with one directory
# per scene: SI-HDR (181 scenes of five Canon 5D Mark III frames, 3 EV apart, CR2,
# the default) and Fairchild's HDR Photographic Survey (105 scenes of nine Nikon D2X
# frames, 1 EV apart, NEF). For every pair of frames (B brighter,
# D darker) of a scene it reports how much B clips, and whether D can serve as
# ground truth there: D must not clip where B does, and D scaled to B's exposure
# must predict B's clipping (moving clouds, water or leaves break that).
import sys, os, json, glob
import numpy as np
import multiprocessing as mp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bracket_scan import quads, blockmean

EXIFTOOL = ['perl', '/tmp/exiftool-dl/Image-ExifTool-13.59/exiftool']


def exposure_times(src, ext):
    import subprocess
    out = subprocess.run(EXIFTOOL + ['-json', '-n', '-r', '-ExposureTime', '-Model', '-ext', ext, src],
                         capture_output=True, text=True, check=True).stdout
    return {os.path.realpath(e['SourceFile']): (float(e['ExposureTime']), e['Model']) for e in json.loads(out)}


def pair_stats(qd, cd, qb, cb, wb):
    gd, gb = blockmean(qd[..., 1]), blockmean(qb[..., 1])
    okb = blockmean(cb.any(2).astype(np.float32)) == 0
    sel = okb & (gd > 0.003) & (gb > 0.02)
    if sel.sum() <= 20:
        return None
    lr = np.log2(gb[sel] / gd[sel])
    ev = float(np.median(lr))
    clipb = cb.any(2)
    n = clipb.sum()
    res = dict(ev=ev, ev_spread=float(np.percentile(np.abs(lr - ev), 90)),
               b_clipped=float(clipb.mean()), b_all3=float(cb.all(2).mean()))
    if n == 0:
        return res
    truth = qd * 2 ** ev                       # D at B's exposure, in units of the clip level
    tmax = truth.max(2)
    res.update(
        # where B clips but D clips too, there is no ground truth
        d_clipped_in_region=float(cd.any(2)[clipb].mean()),
        # how far above the clip level the truth goes, where B clips
        deep=float((tmax[clipb] > 2).mean()), deep_x8=float((tmax[clipb] > 8).mean()),
        # B's clipping predicted from D; low agreement means motion or misalignment
        clip_agreement=float(((truth >= 1).any(2) & clipb).sum() / max(((truth >= 1).any(2) | clipb).sum(), 1)))
    ok = clipb & ~cd.any(2)
    rgb = qd[ok] * wb[None, :].astype(np.float32)
    if len(rgb):
        mx = rgb.max(1)
        chroma = (mx - rgb.min(1)) / np.maximum(mx, 1e-6)
        res.update(colored=float((chroma > 0.3).mean()), chroma_p90=float(np.percentile(chroma, 90)))
    return res


def scene(arg):
    d, tmap, ext = arg
    files = sorted(glob.glob(os.path.join(d, '*.' + ext)))
    try:
        times = [tmap[os.path.realpath(f)][0] for f in files]
        cam = tmap[os.path.realpath(files[0])][1]
        files = [f for _, f in sorted(zip(times, files))]
        times = sorted(times)
        frames = [quads(f, cam) for f in files]
    except Exception as e:
        return dict(scene=os.path.basename(d), error=str(e))
    out = dict(scene=os.path.basename(d), files=[os.path.basename(f) for f in files], times=times, pairs=[])
    for j in range(1, len(frames)):
        qb, cb, wb = frames[j]
        for i in range(j):
            qd, cd, _ = frames[i]
            s = pair_stats(qd, cd, qb, cb, wb)
            if s:
                s.update(dark=i, bright=j)
                out['pairs'].append(s)
    out['frame_clipped'] = [float(f[1].any(2).mean()) for f in frames]
    return out


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    ext = sys.argv[4] if len(sys.argv) > 4 else 'CR2'
    dirs = sorted(d for d in glob.glob(os.path.join(src, '*')) if os.path.isdir(d))
    tmap = exposure_times(src, ext)
    # rawpy's OpenMP can deadlock in forked workers
    mp.set_start_method('spawn')
    with mp.Pool(workers) as p:
        res = p.map(scene, [(d, tmap, ext) for d in dirs if glob.glob(os.path.join(d, '*.' + ext))], chunksize=1)
    json.dump(res, open(dst, 'w'), indent=1)
    print(len(res), 'scenes,', sum('error' in r for r in res), 'errors')
