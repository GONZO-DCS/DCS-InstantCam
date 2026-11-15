"""Generates body.png - the back of a 90s/2000s-style instant camera.
Layout is in a 1024 x 704 design space (same as the shader); output is 1.5x.
Dynamic parts (screen, LCD digits, zoom bar, LED, film counter, button press)
are drawn by the shader on top."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont

S = 1.5
W, H = int(1024 * S), int(704 * S)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
X = (xx + 0.5) / S
Y = (yy + 0.5) / S
rng = np.random.default_rng(42)


def sd_rbox(cx, cy, hx, hy, r):
    qx = np.abs(X - cx) - hx + r
    qy = np.abs(Y - cy) - hy + r
    return np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2) + np.minimum(np.maximum(qx, qy), 0) - r


def sd_circle(cx, cy, r):
    return np.sqrt((X - cx) ** 2 + (Y - cy) ** 2) - r


def fill(d):  # antialiased coverage of d < 0
    return np.clip(0.5 - d * S, 0, 1)


def bevel(d, width):  # 0 at edge -> 1 when width inside
    t = np.clip(-d / width, 0, 1)
    return np.sin(t * np.pi / 2)


def cellular(scale, seed):
    """pebble pattern: distance to nearest feature point (cells of `scale` units)"""
    r = np.random.default_rng(seed)
    gx, gy = X / scale, Y / scale
    ix, iy = np.floor(gx).astype(int), np.floor(gy).astype(int)
    pts = r.random((int(H / S / scale) + 4, int(W / S / scale) + 4, 2))
    best = np.full(X.shape, 9.0, np.float32)
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            cx_, cy_ = ix + ox, iy + oy
            p = pts[np.clip(cy_ + 1, 0, pts.shape[0] - 1), np.clip(cx_ + 1, 0, pts.shape[1] - 1)]
            dx = cx_ + 0.15 + 0.7 * p[..., 0] - gx
            dy = cy_ + 0.15 + 0.7 * p[..., 1] - gy
            best = np.minimum(best, np.sqrt(dx * dx + dy * dy))
    return best


height = np.zeros((H, W), np.float32)
albedo = np.zeros((H, W, 3), np.float32)
rough = np.full((H, W), 0.6, np.float32)  # 0 glossy .. 1 matte

def paint(mask, color, h=None, r=None):
    global albedo, height, rough
    m = mask[..., None]
    albedo = albedo * (1 - m) + m * np.array(color, np.float32)
    if h is not None:
        height = height * (1 - mask) + mask * h
    if r is not None:
        rough = rough * (1 - mask) + mask * r

# ---------------- body shell ----------------
d_body = sd_rbox(512, 352, 508, 348, 84)
body_a = fill(d_body)
# chrome rim
paint(body_a, (0.62, 0.63, 0.66), 2 + 6 * bevel(d_body, 14), 0.25)
# brushed effect on rim
brushed = rng.normal(0, 1, (1, W)).astype(np.float32).repeat(H, 0) * 0.015
albedo += (body_a * brushed)[..., None]

# top plastic band (light grey two-tone)
d_top = np.maximum(sd_rbox(512, 352, 494, 334, 72), Y - 156)
top = fill(d_top)
paint(top, (0.58, 0.58, 0.57), 8 + 3 * bevel(d_top, 6), 0.55)
albedo += (top * rng.normal(0, 0.006, (H, W)).astype(np.float32))[..., None]

# chrome divider line
div = fill(np.abs(Y - 160) - 3) * fill(sd_rbox(512, 352, 494, 334, 72))
paint(div, (0.78, 0.79, 0.81), 9, 0.15)

# leatherette main panel
d_lea = np.maximum(sd_rbox(512, 352, 494, 334, 72), 164 - Y)
lea = fill(d_lea)
peb = cellular(5.5, 7)
pebble_h = np.clip(1 - peb / 0.75, 0, 1) ** 0.7
paint(lea, (0.022, 0.022, 0.024), 7 + 1.6 * pebble_h + 2 * bevel(d_lea, 5), 0.7)
albedo += (lea * (pebble_h - 0.5) * 0.02)[..., None]

# ---------------- top band details ----------------
# print eject slot
d_slot = sd_rbox(512, 30, 190, 7, 6)
paint(fill(d_slot + 3), (0.10, 0.10, 0.10), 9, 0.6)
paint(fill(d_slot), (0.01, 0.01, 0.01), 3, 0.9)
# vent slots (left)
for i in range(6):
    d = sd_rbox(70 + i * 26, 92, 6, 24, 6)
    paint(fill(d), (0.30, 0.30, 0.30), 6 - 3 * bevel(d, 5), 0.8)
# shutter button chrome ring + red dome
d_ring = sd_circle(900, 82, 46)
paint(fill(d_ring), (0.80, 0.81, 0.83), 10 + 6 * bevel(d_ring, 8), 0.12)
d_btn = sd_circle(900, 82, 35)
dome = np.sqrt(np.clip(1 - ((X - 900) ** 2 + (Y - 82) ** 2) / 35 ** 2, 0, 1))
paint(fill(d_btn), (0.62, 0.07, 0.04), 12 + 10 * dome, 0.2)
# LED window
d_led = sd_circle(815, 82, 14)
paint(fill(d_led), (0.25, 0.25, 0.26), 10 + 3 * bevel(d_led, 4), 0.2)
paint(fill(d_led + 4), (0.02, 0.02, 0.02), 9, 0.1)

# ---------------- viewfinder screen housing ----------------
d_vf = sd_rbox(300, 412, 238, 236, 40)
paint(fill(d_vf), (0.020, 0.020, 0.022), 9 + 6 * bevel(d_vf, 16), 0.18)
d_scr = sd_rbox(300, 412, 204, 204, 14)
paint(fill(d_scr + 3), (0.005, 0.005, 0.006), 11, 0.1)
# small embossed marks around the viewfinder
for (mx, my) in [(110, 196), (490, 196), (110, 628), (490, 628)]:
    pass

# ---------------- LCD panel ----------------
d_lcdf = sd_rbox(785, 260, 186, 70, 12)
paint(fill(d_lcdf), (0.70, 0.71, 0.73), 8 + 3 * bevel(d_lcdf, 5), 0.2)
d_lcd = sd_rbox(785, 258, 172, 54, 6)
paint(fill(d_lcd), (0.40, 0.45, 0.36), 7.5, 0.15)
# zoom bar slot
d_zb = sd_rbox(785, 342, 172, 7, 6)
paint(fill(d_zb), (0.06, 0.06, 0.06), 6.5, 0.5)

# ---------------- film counter window ----------------
d_fc = sd_circle(690, 482, 54)
paint(fill(d_fc), (0.78, 0.79, 0.81), 9 + 5 * bevel(d_fc, 8), 0.12)
d_fcg = sd_circle(690, 482, 40)
paint(fill(d_fcg), (0.012, 0.010, 0.010), 9, 0.05)
# tick marks around counter
for k in range(10):
    a = -np.pi / 2 + k * 2 * np.pi / 10
    tx, ty = 690 + 64 * np.cos(a), 482 + 64 * np.sin(a)
    paint(fill(sd_circle(tx, ty, 2.6)), (0.65, 0.65, 0.66), 8, 0.4)

# ---------------- grip ribs ----------------
for i in range(9):
    x0 = 800 + i * 20
    d = sd_rbox(x0, 520, 6, 118, 6)
    prof = np.sqrt(np.clip(1 - ((X - x0) / 6.5) ** 2, 0, 1))
    paint(fill(d), (0.07, 0.07, 0.075), 8 + 4 * prof, 0.45)

# ---------------- lighting ----------------
gx = np.gradient(height, axis=1) * S
gy = np.gradient(height, axis=0) * S
n = np.dstack([-gx, -gy, np.full_like(height, 2.2)])
n /= np.linalg.norm(n, axis=2, keepdims=True)
L = np.array([-0.45, -0.6, 0.66]); L /= np.linalg.norm(L)
V = np.array([0, 0, 1.0])
Hv = (L + V) / np.linalg.norm(L + V)
ndl = np.clip((n * L).sum(-1), 0, 1)
ndh = np.clip((n * Hv).sum(-1), 0, 1)
shin = 8 + (1 - rough) * 120
spec = (ndh ** shin) * (1 - rough) * 0.9
amb = 0.38 + 0.12 * (1 - Y / 704)  # soft top light
lit = albedo * (amb[..., None] + 0.75 * ndl[..., None]) + spec[..., None]

# LCD lettering (printed, static)
alpha = body_a
img = np.clip(lit, 0, 1) ** (1 / 2.2)
rgba = np.dstack([img * 255, alpha * 255]).astype(np.uint8)
im = Image.fromarray(rgba, "RGBA")
dr = ImageDraw.Draw(im)
fnt = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", int(26 * S))
dr.text((int(882 * S), int(268 * S)), "mm", font=fnt, fill=(30, 36, 28, 255))
small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", int(13 * S))
dr.text((int(625 * S), int(214 * S)), "FOCAL", font=small, fill=(40, 48, 36, 255))
dr.text((int(660 * S), int(548 * S)), "FILM", font=small, fill=(150, 150, 152, 255))
dr.text((int(612 * S), int(362 * S)), "W", font=small, fill=(150, 150, 152, 255))
dr.text((int(946 * S), int(362 * S)), "T", font=small, fill=(150, 150, 152, 255))
im.save("pkg/body.png")
print("body.png", im.size)
