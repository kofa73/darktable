# wb_sidecar.py <in.xmp> <out.xmp> <r> <g> <b> <preset>
# Appends a temperature v4 history item to a sidecar written by make_sidecar.py.
# preset: 0 as shot, 3 camera reference, 4 as shot to reference (the scene-referred default).
# Without this item darktable applies the workflow default, "as shot to reference".
import re, struct, sys
src, dst, r, g, b, preset = sys.argv[1], sys.argv[2], *map(float, sys.argv[3:6]), int(sys.argv[6])
s = open(src).read()
n = int(re.search(r'darktable:history_end="(\d+)"', s).group(1))
params = struct.pack('<ffffi', r, g, b, 0.0, preset).hex()
li = ('     <rdf:li darktable:num="%d" darktable:operation="temperature" darktable:enabled="1" darktable:modversion="4"'
      ' darktable:params="%s" darktable:multi_name="" darktable:multi_name_hand_edited="0"'
      ' darktable:multi_priority="0" darktable:blendop_version="14"'
      ' darktable:blendop_params="gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU="/>\n' % (n, params))
s = s.replace('    </rdf:Seq>\n   </darktable:history>', li + '    </rdf:Seq>\n   </darktable:history>')
s = s.replace('darktable:history_end="%d"' % n, 'darktable:history_end="%d"' % (n + 1))
open(dst, 'w').write(s)
