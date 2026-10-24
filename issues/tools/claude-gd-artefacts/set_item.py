import re, sys
# set_item.py <in.xmp> <out.xmp> <history num> <hex params>: replaces the params of one history item
src = open(sys.argv[1]).read()
num, params = sys.argv[3], sys.argv[4]
def fix(m):
    li = m.group(0)
    if 'darktable:num="%s"' % num in li:
        li = re.sub(r'darktable:params="[^"]*"', 'darktable:params="%s"' % params, li)
    return li
out = re.sub(r'<rdf:li\s+darktable:num=.*?/>', fix, src, flags=re.S)
assert params in out
open(sys.argv[2], 'w').write(out)
