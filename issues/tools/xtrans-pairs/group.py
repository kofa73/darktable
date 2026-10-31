# group.py: bursts of the X-Trans raws by capture time (gap <= 4 s between frames), as directories of
# symlinks in scenes/, so that nothing is written next to Kofa's originals.
import subprocess, os, csv, datetime as D
T = '/tmp/hl-rules-study/tasks/2026-10-31-xtrans/scenes'
src = ['/tmp/gradient-domain-eval/raw/x-trans-pack', '/tmp/gradient-domain-eval/bracketed/x-trans']
files = sum([[os.path.join(d, f) for f in sorted(os.listdir(d))] for d in src], [])
out = subprocess.run(['exiftool', '-q', '-T', '-Directory', '-FileName', '-DateTimeOriginal', '-Model'] + files, capture_output=True, text=True).stdout
rows = sorted((l.split('\t') for l in out.splitlines()), key=lambda r: r[2])
groups, last = [], None
for d, f, t, m in rows:
    tt = D.datetime.strptime(t, '%Y:%m:%d %H:%M:%S')
    if last is None or (tt - last).total_seconds() > 4 or groups[-1][0][3] != m:
        groups.append([])
    groups[-1].append((d, f, tt, m)); last = tt
n = 0
for g in groups:
    if len(g) < 2:
        continue
    name = 'x%02d-%s-%s' % (n, g[0][3].lower().replace('-', ''), g[0][2].strftime('%m%d-%H%M%S')); n += 1
    os.makedirs(os.path.join(T, name), exist_ok=True)
    for d, f, _, _ in g:
        l = os.path.join(T, name, f)
        if not os.path.lexists(l):
            os.symlink(os.path.join(d, f), l)
print(len(groups), 'groups,', n, 'with 2+ frames')
