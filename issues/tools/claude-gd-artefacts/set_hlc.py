# writes <out> = <in> with the write-back and experiment of every hlcolor item set
import re, struct, sys
src, dst, wb, exp = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
s = open(src).read()
def item(m):
    li = m.group(0)
    if 'darktable:operation="hlcolor"' not in li: return li
    pm = re.search(r'darktable:params="([0-9a-f]+)"', li)
    p = bytes.fromhex(pm.group(1))[:4] + struct.pack('<ii', wb, exp)
    return li[:pm.start(1)] + p.hex() + li[pm.end(1):]
s = re.sub(r'<rdf:li\s+darktable:num=.*?/>', item, s, flags=re.S)
open(dst, 'w').write(s)
