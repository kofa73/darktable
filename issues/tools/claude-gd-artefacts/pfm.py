import numpy as np
def read_pfm(path):
    with open(path, 'rb') as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        ch = 3 if kind == b'PF' else 1
        a = np.fromfile(f, '<f4' if scale < 0 else '>f4', w * h * ch).reshape(h, w, ch)
    return a[::-1]
def to_png(a, path, crop=None, zoom=1):
    from PIL import Image
    if crop: x, y, w, h = crop; a = a[y:y+h, x:x+w]
    a = np.clip(a, 0, 1)
    s = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    im = Image.fromarray((s * 255 + 0.5).astype(np.uint8))
    if zoom != 1: im = im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)
    im.save(path)
