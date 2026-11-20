from PIL import Image, ImageDraw, ImageFont
import sys

SIZE = 1024
BG = (0x35, 0x28, 0x79)          # C64 blue
# the colours of the lettering (theme "C64", track colours in the order of the title S I D i M I D I)
track = [0xff6b4a, 0x35d6ff, 0xb5f23a, 0xffd43b, 0xf783ac, 0x9775fa, 0x38d9a9, 0xffa94d]
colour_of = [0, 7, 3, 2, 6, 1, 5, 4]
def rgb(v): return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
cols = [rgb(track[i]) for i in colour_of]

def ink(font, c):
    pad = int(font.size * 1.5)
    m = Image.new('L', (pad * 3, pad * 3), 0)
    ImageDraw.Draw(m).text((pad, pad * 2), c, font=font, fill=255, anchor='ls')
    b = m.getbbox()
    return (b[0] - pad, b[1] - pad * 2, b[2] - pad, b[3] - pad * 2)       # relative to the origin on the baseline

def render(size, font_path='C:/Windows/Fonts/segoeuib.ttf'):
    S = 4                                   # draw large, then shrink (smooth edges)
    W = size * S
    img = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, W - 1, W - 1), radius=int(W * 0.2), fill=BG + (255,))
    # a thin lighter rim like the buttons of the program
    d.rounded_rectangle((int(W * 0.012),) * 2 + (W - 1 - int(W * 0.012),) * 2, radius=int(W * 0.19), outline=(0x8f, 0x82, 0xff, 255), width=max(2, int(W * 0.008)))

    fs = int(W * 0.30)
    font = ImageFont.truetype(font_path, fs)
    top, bottom = 'SIDi', 'MIDI'
    # row 2: MIDI as it runs
    adv = [font.getlength(c) for c in bottom]
    box = [ink(font, c) for c in bottom]
    xs = [0.0]
    for a in adv[:-1]: xs.append(xs[-1] + a)
    inkL = xs[0] + box[0][0]
    inkR = xs[-1] + box[-1][2]
    inkW = inkR - inkL
    scale = (W * 0.76) / inkW
    if abs(scale - 1) > 0.01:
        fs = int(fs * scale)
        font = ImageFont.truetype(font_path, fs)
        adv = [font.getlength(c) for c in bottom]
        box = [ink(font, c) for c in bottom]
        xs = [0.0]
        for a in adv[:-1]: xs.append(xs[-1] + a)
        inkL = xs[0] + box[0][0]
        inkR = xs[-1] + box[-1][2]
        inkW = inkR - inkL
    L = (W - inkW) / 2.0
    R = L + inkW
    cap = -ink(font, 'M')[1]          # height of a capital letter
    gap = cap * 0.16
    total = cap * 2 + gap
    y2 = (W - total) / 2.0 + total               # baseline of the lower row
    y1 = y2 - cap - gap                          # baseline of the upper row
    # lower row
    for i, c in enumerate(bottom):
        d.text((L - inkL + xs[i], y2), c, font=font, fill=cols[4 + i] + (255,), anchor='ls')
    # upper row: S flush with the M, i flush with the last I, I and D evenly between
    tb = [ink(font, c) for c in top]
    inkw = [b[2] - b[0] for b in tb]
    free = inkW - sum(inkw)
    g = free / 3.0
    x = L
    for i, c in enumerate(top):
        d.text((x - tb[i][0], y1), c, font=font, fill=cols[i] + (255,), anchor='ls')
        x += inkw[i] + g
    return img.resize((size, size), Image.LANCZOS)

big = render(1024)
out = sys.argv[1]
big.save(out + '.png')
big.resize((256, 256), Image.LANCZOS).save(out + '_256.png')
sizes = [16, 24, 32, 48, 64, 128, 256]
big.save(out + '.ico', sizes=[(s, s) for s in sizes])
print('ok')
