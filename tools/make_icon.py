# make_icon.py - draws the 3DO to PC icon (assets/3do2pc.ico and 3do2pc.png).
# Usage: python tools/make_icon.py assets   (needs Pillow)
import math
from PIL import Image, ImageDraw, ImageFilter, ImageChops
S = 1024
GOLD = (224, 176, 64)

def lerp(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

def rounded_mask(size, box, r):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, r, fill=255)
    return m

def vgrad(size, top, bottom):
    g = Image.new("RGBA", (size, size))
    d = ImageDraw.Draw(g)
    for y in range(size): d.line([(0, y), (size, y)], fill=lerp(top, bottom, y / (size - 1)) + (255,))
    return g

def disc(size, cx, cy, r, simple=False):
    """Iridescent CD: conic rainbow sheen over silver, with hub and hole."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    hues = [(200,210,230),(150,200,255),(170,255,220),(255,245,170),(255,190,220),(190,170,255),(200,210,230)]
    for y in range(int(cy - r), int(cy + r) + 1):
        for x in range(int(cx - r), int(cx + r) + 1):
            dx, dy = x - cx, y - cy
            d = math.hypot(dx, dy)
            if d > r: continue
            a = (math.atan2(dy, dx) / (2 * math.pi) + 0.5) * 2 % 1.0    # two sweeps around
            seg = a * (len(hues) - 1); i = int(seg)
            c = lerp(hues[i], hues[i + 1], seg - i)
            shade = 0.80 + 0.20 * (1 - d / r)
            c = tuple(min(255, int(v * shade)) for v in c)
            px[x, y] = c + (255,)
    dr = ImageDraw.Draw(img)
    dr.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 255, 255, 200), width=max(2, int(r * 0.025)))
    hub = r * (0.36 if not simple else 0.42)
    dr.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=(225, 228, 238, 255), outline=(160, 165, 180, 255), width=max(1, int(r * 0.02)))
    hole = r * (0.15 if not simple else 0.2)
    dr.ellipse([cx - hole, cy - hole, cx + hole, cy + hole], fill=(0, 0, 0, 0))
    # punch the hole fully transparent
    m = Image.new("L", (size, size), 255)
    ImageDraw.Draw(m).ellipse([cx - hole, cy - hole, cx + hole, cy + hole], fill=0)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), m))
    return img

def monitor(size, box, simple=False):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # stand
    sx = (x0 + x1) / 2
    d.polygon([(sx - w*0.10, y1 - 2), (sx + w*0.10, y1 - 2), (sx + w*0.16, y1 + h*0.20), (sx - w*0.16, y1 + h*0.20)], fill=(70, 70, 90, 255))
    d.rounded_rectangle([sx - w*0.30, y1 + h*0.18, sx + w*0.30, y1 + h*0.27], radius=h*0.05, fill=(90, 90, 112, 255))
    # bezel
    d.rounded_rectangle(box, radius=w * 0.07, fill=(46, 46, 60, 255), outline=GOLD + (255,), width=max(2, int(w * 0.035)))
    # screen with a gradient
    pad = w * 0.085
    sb = [x0 + pad, y0 + pad, x1 - pad, y1 - pad]
    scr = vgrad(size, (70, 150, 255), (30, 50, 140))
    m = rounded_mask(size, [int(v) for v in sb], int(w * 0.035))
    img.paste(scr, (0, 0), m)
    if not simple:
        # a little "play" triangle on the screen
        cx, cy, t = (sb[0] + sb[2]) / 2, (sb[1] + sb[3]) / 2, (sb[3] - sb[1]) * 0.28
        d.polygon([(cx - t*0.8, cy - t), (cx - t*0.8, cy + t), (cx + t, cy)], fill=(255, 255, 255, 235))
    return img

def arrow(size, p0, p1, width, head):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    (x0, y0), (x1, y1) = p0, p1
    ang = math.atan2(y1 - y0, x1 - x0)
    bx, by = x1 - head * math.cos(ang), y1 - head * math.sin(ang)
    d.line([p0, (bx, by)], fill=GOLD + (255,), width=int(width))
    d.ellipse([x0 - width/2, y0 - width/2, x0 + width/2, y0 + width/2], fill=GOLD + (255,))
    px, py = -math.sin(ang), math.cos(ang)
    d.polygon([(x1, y1), (bx + px * head * 0.75, by + py * head * 0.75), (bx - px * head * 0.75, by - py * head * 0.75)], fill=GOLD + (255,))
    return img

def shadow(layer, off, blur, alpha=150):
    a = layer.getchannel("A").point(lambda v: v * alpha // 255)
    sh = Image.new("RGBA", layer.size, (0, 0, 0, 0)); sh.putalpha(a)
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", layer.size, (0, 0, 0, 0)); out.paste(sh, off, sh)
    return out

FONT = "/usr/share/fonts/opentype/inter/Inter-Black.otf"
WHITE = (245, 245, 250)

def wordmark(img, parts, box, max_h):
    """Draws coloured text pieces centred in box, as large as fits."""
    from PIL import ImageFont
    x0, y0, x1, y1 = box
    size = int(max_h * 1.45)
    while size > 8:
        f = ImageFont.truetype(FONT, size)
        widths = [f.getlength(t) for t, _ in parts]
        bb = f.getbbox("".join(t for t, _ in parts))
        if sum(widths) <= x1 - x0 and bb[3] - bb[1] <= max_h: break
        size -= 4
    d = ImageDraw.Draw(img)
    total = sum(widths)
    tx = x0 + ((x1 - x0) - total) / 2
    bb = f.getbbox("".join(t for t, _ in parts))
    ty = y0 + ((y1 - y0) - (bb[3] - bb[1])) / 2 - bb[1]
    # soft shadow, then the letters
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0)); sd = ImageDraw.Draw(sh)
    x = tx
    for (t, _), w in zip(parts, widths): sd.text((x + size*0.03, ty + size*0.05), t, font=f, fill=(0, 0, 0, 170)); x += w
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(size * 0.04)))
    x = tx
    for (t, col), w in zip(parts, widths): d.text((x, ty), t, font=f, fill=col + (255,)); x += w

NAME = [("3DO", WHITE), ("2", GOLD), ("PC", WHITE)]

def background(simple):
    base = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bg = vgrad(S, (58, 48, 92), (18, 18, 28))
    edge, rad = (0, 150) if simple else (24, 200)   # small sizes use the whole square
    m = rounded_mask(S, [edge, edge, S - edge, S - edge], rad)
    base.paste(bg, (0, 0), m)
    ImageDraw.Draw(base).rounded_rectangle([edge, edge, S - edge, S - edge], rad, outline=(255, 255, 255, 40), width=6)
    return base

def render(simple=False):
    base = background(simple)
    if simple:
        # Small sizes: just the name, stacked so the letters stay big.
        wordmark(base, [("3DO", WHITE)], (70, 90, S - 70, 500), 360)
        wordmark(base, [("2", GOLD), ("PC", WHITE)], (70, 530, S - 70, 940), 360)
        return base
    # Picture in the upper part, name across the bottom.
    dsc = disc(S, 330, 320, 205)
    mon = monitor(S, (520, 300, 860, 530))
    arr = arrow(S, (520, 200), (690, 300), 40, 82)
    for layer in (dsc, mon, arr):
        base = Image.alpha_composite(base, shadow(layer, (8, 12), 14))
        base = Image.alpha_composite(base, layer)
    wordmark(base, NAME, (80, 690, S - 80, 940), 210)
    return base

full, simple = render(False), render(True)
import sys, os
out = sys.argv[1] if len(sys.argv) > 1 else "."
full.save(os.path.join(out, "3do2pc.png"))
sizes = [256, 128, 64, 48, 32, 24, 16]
frames = [(full if s >= 48 else simple).resize((s, s), Image.LANCZOS) for s in sizes]
frames[0].save(os.path.join(out, "3do2pc.ico"), format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[1:])
# preview sheet on light and dark backgrounds
sheet = Image.new("RGBA", (900, 330), (240, 240, 240, 255))
dark = Image.new("RGBA", (900, 165), (32, 32, 32, 255)); sheet.paste(dark, (0, 165))
for row, y in ((0, 0), (1, 165)):
    x = 10
    for f in frames:
        sheet.alpha_composite(f, (x, y + (165 - f.size[1]) // 2)); x += f.size[0] + 14
sheet.save("/tmp/icon_preview.png")
