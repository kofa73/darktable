import sys
# writes the XMP packet embedded in a JPEG to a sidecar file
data = open(sys.argv[1], 'rb').read()
s = data.find(b'<x:xmpmeta')
e = data.find(b'</x:xmpmeta>', s) + len(b'</x:xmpmeta>')
open(sys.argv[2], 'wb').write(b'<?xml version="1.0" encoding="UTF-8"?>\n' + data[s:e] + b'\n')
