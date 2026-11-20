"""paper.png - worn instant-film card (88 x 107 mm, 79 x 79 mm window) at photo size 1024."""
import numpy as np
from PIL import Image, ImageFilter
SIZE = 1536
W, H = round(SIZE * 88 / 79), round(SIZE * 107 / 79)
rng = np.random.default_rng(11)

def smooth_noise(scale, seed):
    r = np.random.default_rng(seed)
    small = r.random((max(2, H // scale), max(2, W // scale))).astype(np.float32)
    im = Image.fromarray((small * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC)
    return np.asarray(im).astype(np.float32) / 255

base = np.array([236, 234, 227], np.float32)
img = np.zeros((H, W, 3), np.float32) + base
img += ((smooth_noise(40, 1) - 0.5) * 10)[..., None]             # mottling
img += ((smooth_noise(6, 2) - 0.5) * 6)[..., None]               # fibre
img += (rng.normal(0, 2.2, (H, W)))[..., None]                   # grain
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
# darker, slightly yellowed worn edges
edge = np.minimum.reduce([xx, yy, W - 1 - xx, H - 1 - yy])
wear = np.clip(1 - edge / 42, 0, 1) ** 2 * (0.6 + 0.4 * smooth_noise(12, 3))
img -= (wear * 38)[..., None] * np.array([1.0, 1.05, 1.25])
# chipped edge specks
chips = (smooth_noise(3, 4) > 0.82) & (edge < 15)
img[chips] -= 45
# grey grime smudges (a few soft blotches)
for i in range(7):
    cx, cy = rng.uniform(0, W), rng.uniform(0, H)
    if i < 2:  # one near the bottom of the image window like a thumb mark
        cx, cy = rng.uniform(W * 0.35, W * 0.65), SIZE * 85 / 79 + rng.uniform(-10, 25)
    rx, ry = rng.uniform(38, 135), rng.uniform(15, 60)
    d = ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2
    m = np.clip(1 - d, 0, 1) ** 1.5 * smooth_noise(5, 10 + i)
    img -= (m * rng.uniform(25, 55))[..., None]
img = np.clip(img, 0, 255).astype(np.uint8)
Image.fromarray(img, "RGB").convert("RGBA").save("pkg/paper.png")
print(W, H)
