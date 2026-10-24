import sys
import numpy as np
import colour
from PIL import Image
# a* and L* profile across the right edge of the middle tower, averaged over rows
def lab(path):
    rgb = np.asarray(Image.open(path).convert('RGB'), np.float32) / 255
    return colour.XYZ_to_Lab(colour.sRGB_to_XYZ(rgb))
E = '/tmp/gradient-domain-eval/raw/darktable_exported/DSC_0535_'
for name in sys.argv[1:]:
    L = lab(E + name + '.jpg')
    band = L[1150:1250, 1110:1400].mean(0)
    print(name)
    print('  x     ' + ' '.join('%5d' % x for x in range(1110, 1400, 15)))
    for k, lab_name in ((0, 'L*'), (1, 'a*'), (2, 'b*')):
        print('  %-5s ' % lab_name + ' '.join('%5.1f' % v for v in band[::15, k]))
    # false-color map of a* around the tower
    a = L[950:1400, 750:1450, 1]
    img = np.clip((a + 10) / 30, 0, 1)
    Image.fromarray((np.stack([img, 1 - img, 1 - img], -1) * 255).astype(np.uint8)).save('astar-%s.png' % name)
