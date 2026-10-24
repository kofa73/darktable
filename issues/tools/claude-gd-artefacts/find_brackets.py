#!/usr/bin/env python3
"""Find exposure brackets: groups of raw files from one camera in which each
frame was shot within a short gap of the previous one, and whose exposures differ.

usage: find_brackets.py [options] DIR [DIR ...]

Needs exiftool (https://exiftool.org) in PATH, or its path in --exiftool.
A long scan can be kept with --cache FILE: the first run writes the metadata
there, later runs read it instead of rescanning, so other settings are cheap.

Output, one block per group, sorted by time:
  2023-05-01 10:30:46.75  NIKON D7200 #9872783  3 frames, spread 4.0 EV
     +0.0 EV  1/500 s  f/8  ISO 100  /photos/a.nef
     +2.0 EV  1/125 s  f/8  ISO 100  /photos/b.nef
     ...
EV is the exposure relative to the darkest frame of the group: log2 of
exposure time x ISO / f-number^2. The darkest frame is the one most likely
to have nothing clipped.
"""
import argparse, json, math, os, subprocess, sys
from collections import defaultdict
from datetime import datetime, timedelta

RAW_EXT = ('3fr arw cr2 cr3 crw dcr dng erf fff iiq k25 kdc mef mos mrw nef nrw '
           'orf ori pef raf raw rw2 rwl sr2 srf srw x3f').split()
# the '#' suffix makes exiftool print numbers instead of "1/250" or "f/8"
TAGS = ['-DateTimeOriginal', '-SubSecTimeOriginal', '-ExposureTime#', '-FNumber#', '-ISO#',
        '-Model', '-SerialNumber', '-FileSize#']


def scan(exiftool, dirs, exts):
    cmd = [exiftool, '-r', '-json', '-q', '-q', '-fast']
    for e in exts:
        cmd += ['-ext', e]
    res = subprocess.run(cmd + TAGS + dirs, stdout=subprocess.PIPE)
    # exiftool exits with 1 when some files could not be read or none matched;
    # the JSON it printed for the other files is still valid
    return json.loads(res.stdout) if res.stdout.strip() else []


def timestamp(r):
    try:
        dt = datetime.strptime(str(r.get('DateTimeOriginal', ''))[:19], '%Y:%m:%d %H:%M:%S')
    except ValueError:
        return None
    t = (dt - datetime(1970, 1, 1)).total_seconds()
    # SubSecTimeOriginal holds the digits after the decimal point, so "075" is
    # 0.075 s. exiftool's JSON turns "75" into the number 75; str() restores it.
    # Without it (many Olympus and Panasonic raws) times have whole seconds only
    sub = str(r.get('SubSecTimeOriginal', '')).strip()
    if sub.isdigit():
        t += float('0.' + sub)
    return t


def iso_of(r):
    # some Nikons store ISO as two numbers, which exiftool prints as "1 8000";
    # the last one is the ISO
    v = r.get('ISO')
    if isinstance(v, str):
        try:
            v = float(v.split()[-1])
        except (ValueError, IndexError):
            return None
    return v if isinstance(v, (int, float)) and v > 0 else None


def stops(r):
    # log2 of the light the sensor got, up to a constant; None if unknown
    t, n, iso = r.get('ExposureTime'), r.get('FNumber'), iso_of(r)
    # without the ISO, frames of an auto-ISO sequence would look a bracket apart
    if not isinstance(t, (int, float)) or t <= 0 or iso is None:
        return None
    s = math.log2(t)
    # lenses without electronic contacts report no f-number: assume it did not change
    if isinstance(n, (int, float)) and n > 0:
        s -= 2 * math.log2(n)
    return s + math.log2(iso)


def merge_copies(recs):
    # a collection often holds the same raw twice (backups, renamed copies);
    # counted twice, a 2-frame bracket would pass as a 4-frame group
    frames = {}
    for r in recs:
        key = (r.get('Model'), r.get('SerialNumber'), r['t'], r.get('ExposureTime'),
               r.get('FNumber'), r.get('ISO'), r.get('FileSize'))
        if key in frames:
            frames[key]['copies'].append(r['SourceFile'])
        else:
            r['copies'] = []
            frames[key] = r
    return list(frames.values())


def ev100(r):
    # exposure value at ISO 100 that the camera used: about 3-7 for night
    # streets, 5-8 indoors, 12-15 in daylight; None if unknown
    t, n, iso = r.get('ExposureTime'), r.get('FNumber'), iso_of(r)
    if not all(isinstance(v, (int, float)) and v > 0 for v in (t, n, iso)):
        return None
    return math.log2(n * n / t) - math.log2(iso / 100)


def groups(recs, gap, add_exposure):
    by_camera = defaultdict(list)
    for r in recs:
        by_camera[(str(r.get('Model', '?')), str(r.get('SerialNumber', '')))].append(r)
    for camera, rs in by_camera.items():
        rs.sort(key=lambda r: r['t'])
        cur = [rs[0]]
        for prev, r in zip(rs, rs[1:]):
            allowed = gap
            # the time stamp is taken when the exposure starts, so a long frame
            # delays the next one by its own length
            if add_exposure and isinstance(prev.get('ExposureTime'), (int, float)):
                allowed += prev['ExposureTime']
            if r['t'] - prev['t'] <= allowed:
                cur.append(r)
            else:
                yield camera, cur
                cur = [r]
        yield camera, cur


def shutter(t):
    if not isinstance(t, (int, float)) or t <= 0:
        return '? s'
    return '1/%d s' % round(1 / t) if t < 0.5 else '%g s' % round(t, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dirs', nargs='*', help='directories to scan, recursively')
    ap.add_argument('--gap', type=float, default=1.0,
                    help='largest time between consecutive frames, in seconds (default 1)')
    ap.add_argument('--add-exposure', action='store_true',
                    help='also allow the exposure time of the earlier frame, for long night brackets')
    ap.add_argument('--min-frames', type=int, default=3, help='smallest group to report (default 3)')
    ap.add_argument('--min-spread', type=float, default=1.0,
                    help='smallest exposure difference in a group, in EV (default 1; 0 also lists bursts)')
    ap.add_argument('--max-scene-ev', type=float,
                    help='only groups whose median frame has at most this EV100, e.g. 8 for night and indoor scenes')
    ap.add_argument('--ext', default=','.join(RAW_EXT),
                    help='comma-separated file extensions (default: common raw formats)')
    ap.add_argument('--cache', help='JSON file to keep the scanned metadata in')
    ap.add_argument('--exiftool', default='exiftool', help='exiftool command (default exiftool)')
    a = ap.parse_args()

    if a.cache and os.path.exists(a.cache):
        with open(a.cache) as f:
            recs = json.load(f)
    else:
        if not a.dirs:
            ap.error('no directories to scan')
        try:
            recs = scan(a.exiftool, a.dirs, a.ext.split(','))
        except FileNotFoundError:
            sys.exit('%s not found; install it or pass its path with --exiftool' % a.exiftool)
        if a.cache:
            with open(a.cache, 'w') as f:
                json.dump(recs, f)

    dated = []
    for r in recs:
        r['t'] = timestamp(r)
        if r['t'] is not None:
            dated.append(r)
    frames = merge_copies(dated)
    print('%d files, %d with a capture time, %d distinct frames' % (len(recs), len(dated), len(frames)),
          file=sys.stderr)

    found = []
    for (model, serial), g in groups(frames, a.gap, a.add_exposure):
        if len(g) < a.min_frames:
            continue
        s = [stops(r) for r in g]
        known = [v for v in s if v is not None]
        spread = max(known) - min(known) if known else 0.0
        if spread < a.min_spread:
            continue
        evs = sorted(v for v in (ev100(r) for r in g) if v is not None)
        scene = evs[len(evs) // 2] if evs else None
        if a.max_scene_ev is not None and scene is not None and scene > a.max_scene_ev:
            continue
        found.append((g[0]['t'], model, serial, g, s, spread, scene))

    for t0, model, serial, g, s, spread, scene in sorted(found, key=lambda x: x[0]):
        when = (datetime(1970, 1, 1) + timedelta(seconds=t0)).strftime('%Y-%m-%d %H:%M:%S.%f')[:22]
        print('%s  %s%s  %d frames, spread %.1f EV, scene EV100 %s'
              % (when, model, ' #' + serial if serial else '', len(g), spread,
                 '%.1f' % scene if scene is not None else '?'))
        base = min((v for v in s if v is not None), default=0.0)
        for r, v in zip(g, s):
            ev = '%+5.1f EV' % (v - base) if v is not None else '    ? EV'
            n = r.get('FNumber')
            print('   %s  %-9s %-6s ISO %-6s %s' % (ev, shutter(r.get('ExposureTime')),
                  'f/%g' % n if isinstance(n, (int, float)) and n > 0 else 'f/?',
                  '%g' % iso_of(r) if iso_of(r) else '?', r['SourceFile']))
            for c in r['copies']:
                print('%40s %s' % ('copy:', c))
        print()
    print('%d groups' % len(found), file=sys.stderr)


if __name__ == '__main__':
    main()
