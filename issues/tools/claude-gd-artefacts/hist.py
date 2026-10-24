import re, sys, zlib, base64
# prints the history entries for the given operations from the XMP embedded in a JPEG or a sidecar
data = open(sys.argv[1], 'rb').read().decode('latin-1')
ops = sys.argv[2].split(',') if len(sys.argv) > 2 else None
m = re.search(r'darktable:history_end="(\d+)"', data)
print('history_end', m.group(1) if m else None)
for li in re.findall(r'<rdf:li\s+darktable:num=.*?/>', data, re.S):
    a = dict(re.findall(r'darktable:(\w+)="([^"]*)"', li))
    if ops and a['operation'] not in ops: continue
    p = a['params']
    if p.startswith('gz'):
        raw = zlib.decompress(base64.b64decode(p[4:]))
    else:
        raw = bytes.fromhex(p)
    print(a['num'], a['operation'], 'en', a['enabled'], 'v', a['modversion'], repr(a.get('multi_name')), 'prio', a['multi_priority'], raw.hex())
