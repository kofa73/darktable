# dt_hl_method.py <jpg>...: the highlight reconstruction settings of a darktable export, from the
# history embedded in its XMP (exiftool JSON on stdin is not used; exiftool is called per file).
# Prints, for the last enabled highlights entry within history_end: method, and for gradient
# domain the write-back and experiment values (params v6: '<ifffiiffifiii').
import sys, json, subprocess, struct, zlib, base64
ET = ['perl', '/tmp/exiftool-dl/Image-ExifTool-13.59/exiftool']
MODES = {0: 'clip highlights', 1: 'reconstruct in LCh', 2: 'reconstruct color', 3: 'guided laplacians',
         4: 'segmentation based', 5: 'inpaint opposed', 6: 'gradient domain'}
def params(p):
    if p.startswith('gz'):
        return zlib.decompress(base64.b64decode(p[4:]))
    return bytes.fromhex(p)
for f in sys.argv[1:]:
    d = json.loads(subprocess.run(ET + ['-j', '-struct', '-XMP-darktable:all', f], capture_output=True, text=True).stdout)[0]
    hist = d.get('History', [])
    end = int(d.get('HistoryEnd', len(hist)))
    hl = [h for h in hist[:end] if h.get('Operation') == 'highlights']
    if not hl:
        print(f, 'no highlights entry'); continue
    h = hl[-1]; b = params(h['Params'])
    v = struct.unpack('<ifffiiffifiii', b[:52]) if len(b) >= 52 else struct.unpack('<i', b[:4])
    s = MODES.get(v[0], v[0])
    if v[0] == 6 and len(v) > 12:
        s += ', full res %d, write-back %d, experiment %d' % (v[10], v[11], v[12])
    if v[0] == 4 and len(v) > 8:
        s += ', candidating %.2f, combine %.1f, rebuild %d, strength %.2f' % (v[6], v[7], v[8], v[1])
    print('%s: enabled %s, version %s, %s' % (f.split('/')[-1], h.get('Enabled'), h.get('ModVersion'), s))
