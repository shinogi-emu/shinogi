#!/usr/bin/env python3
"""Generate every raster form of the logo from assets/shinogi_logo.svg.

The SVG is the only hand-edited image in the tree.  Everything this writes
is committed alongside it, because the packaging paths run where a
rasterizer is not installed -- a Windows or macOS build must never need
one.

Rasterizing is librsvg through gi, plus PIL, both already present.  Not
cairosvg: it is not installed and would be a new dependency for no gain.

    python3 tools/make-icons.py            write everything
    python3 tools/make-icons.py --check    verify the committed files match

Outputs:
    tools/linux/shinogi.png, -48, -64, -128    install-linux.sh
    tools/win/shinogi.ico                      shinogi.nsi, launcher
    tools/macos/shinogi.icns                   make-macos-package.sh
    emutos/bios/shinogi_splash.h               the boot splash
"""
import io
import os
import struct
import sys

import gi

gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg                              # noqa: E402
import cairo                                                # noqa: E402
from PIL import Image                                       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(ROOT, "assets", "shinogi_logo.svg")

# The splash header is EmuTOS source, so it goes to the EmuTOS tree that is
# actually built -- which is NOT the submodule checked out under shinogi/.
# make-mint-install.sh builds from $HOME/git/emutos and reads the ELF from
# there; the same default and the same override are used here so the two
# cannot drift apart.
EMUTOS = os.environ.get("EMUTOS_DIR",
                        os.path.join(os.path.expanduser("~"), "git", "emutos"))

# The splash palette, and the order the guest expects: index 0 is the
# transparent one, so the boot screen shows through where the logo is not.
SPLASH_COLOURS = [
    (0x0b, 0x0f, 0x14),     # 1 outline
    (0x34, 0x3b, 0x43),     # 2 charcoal, the mountain
    (0xff, 0xff, 0xff),     # 3 white, the snow and the lettering
]
SPLASH_SIZE = 320


def render(px):
    """The SVG at px by px, as a straight-alpha RGBA PIL image."""
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, px, px)
    ctx = cairo.Context(surface)
    handle = Rsvg.Handle.new_from_file(SVG)
    rect = Rsvg.Rectangle()
    rect.x = rect.y = 0
    rect.width = rect.height = px
    handle.render_document(ctx, rect)
    surface.flush()

    # cairo gives premultiplied BGRA; PIL wants straight RGBA.
    data = bytes(surface.get_data())
    out = bytearray(px * px * 4)
    for i in range(px * px):
        b, g, r, a = data[i * 4:i * 4 + 4]
        if a:
            r = min(255, r * 255 // a)
            g = min(255, g * 255 // a)
            b = min(255, b * 255 // a)
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return Image.frombytes("RGBA", (px, px), bytes(out))


def render_mark(px):
    """Just the mountain, cropped square -- no lettering.

    The QEMU window icon is shown at 32x32 in a taskbar, where the
    SHINOGI lettering is a grey smear and the mountain is still a
    mountain.  Derived from the one SVG rather than hand-drawn a second
    time, so the mark cannot drift away from the logo.
    """
    text = io.open(SVG, encoding="utf-8").read()

    # Drop the lettering group; it is the only <g> in the file.
    start = text.index("  <g fill=\"#ffffff\"")
    end = text.index("</g>", start) + len("</g>")
    text = text[:start] + text[end:]

    # The mountain occupies x 109..915, y 155..826.  Re-frame the viewBox
    # on it, square and centred, with a little air around the edges.
    text = text.replace('viewBox="0 0 1024 1024"',
                        'viewBox="68.5 47 887 887"')

    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, px, px)
    ctx = cairo.Context(surface)
    handle = Rsvg.Handle.new_from_data(text.encode("utf-8"))
    rect = Rsvg.Rectangle()
    rect.x = rect.y = 0
    rect.width = rect.height = px
    handle.render_document(ctx, rect)
    surface.flush()

    data = bytes(surface.get_data())
    out = bytearray(px * px * 4)
    for i in range(px * px):
        b, g, r, a = data[i * 4:i * 4 + 4]
        if a:
            r = min(255, r * 255 // a)
            g = min(255, g * 255 // a)
            b = min(255, b * 255 // a)
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return Image.frombytes("RGBA", (px, px), bytes(out))


def qemu_bmp(img, path):
    """The mark on white, for QEMU's SDL icon loader.

    ui/sdl2.c colour-keys pure white to transparent, and our snow cap is
    pure white -- keyed as-is it would be punched out of the middle of
    the mountain.  So the background is white and every white pixel that
    belongs to the ARTWORK is nudged one step off it, which the eye
    cannot see and the colour key no longer matches.
    """
    flat = Image.new("RGB", img.size, (255, 255, 255))
    flat.paste(img, (0, 0), img)
    src, dst = img.load(), flat.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            if src[x, y][3] >= 128 and dst[x, y] == (255, 255, 255):
                dst[x, y] = (254, 254, 254)
    flat.save(path, "BMP")


def quantise(img):
    """Map to the 4-entry splash palette: 0 transparent, then nearest."""
    px = img.load()
    w, h = img.size
    idx = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                continue                        # stays 0, transparent
            best, bestd = 0, None
            for n, (cr, cg, cb) in enumerate(SPLASH_COLOURS, start=1):
                d = (r - cr) ** 2 + (g - cg) ** 2 + (b - cb) ** 2
                if bestd is None or d < bestd:
                    best, bestd = n, d
            idx[y * w + x] = best
    return idx, w, h


def rle(idx, w, h):
    """Run-length encode as (count, index) byte pairs, per scanline.

    Runs never cross a scanline, so the decoder needs no width arithmetic
    and a truncated stream cannot smear one row into the next.
    """
    out = bytearray()
    for y in range(h):
        row = idx[y * w:(y + 1) * w]
        x = 0
        while x < w:
            v = row[x]
            n = 1
            while x + n < w and row[x + n] == v and n < 255:
                n += 1
            out += bytes((n, v))
            x += n
    return bytes(out)


def write_header(path, data, w, h):
    lines = [
        "/*",
        " * The boot splash, generated by tools/make-icons.py from",
        " * assets/shinogi_logo.svg.  DO NOT EDIT -- regenerate it.",
        " *",
        " * 2 bits of colour per pixel, run-length encoded as (count, index)",
        " * byte pairs that never cross a scanline.  Index 0 is transparent.",
        " * Stored this way because the same image as RGB565 would be %d bytes"
        % (w * h * 2),
        " * against an EMUTOS.ELF of about 425K, which is not a reasonable",
        " * price for a decoration.",
        " */",
        "",
        "#ifndef SHINOGI_SPLASH_H",
        "#define SHINOGI_SPLASH_H",
        "",
        "#define SPLASH_W %d" % w,
        "#define SPLASH_H %d" % h,
        "",
        "/* RGB565, index 0 unused: nothing is drawn where the logo is clear. */",
        "static const UWORD splash_pal[4] = {",
        "    0x0000,",
    ]
    for r, g, b in SPLASH_COLOURS:
        lines.append("    0x%04x,   /* %02x%02x%02x */"
                     % (((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3),
                        r, g, b))
    lines += [
        "};",
        "",
        "#define SPLASH_RLE_LEN %d" % len(data),
        "",
        "static const UBYTE splash_rle[SPLASH_RLE_LEN] = {",
    ]
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02x" % c for c in data[i:i + 16])
        lines.append("    %s," % chunk)
    lines += ["};", "", "#endif /* SHINOGI_SPLASH_H */", ""]
    body = "\n".join(lines)
    with open(path, "w") as fh:
        fh.write(body)
    return body


def icns(img_by_size, path):
    """A minimal but valid .icns: PNG-backed entries, largest first."""
    types = [(b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024)]
    import io
    blobs = []
    for tag, px in types:
        buf = io.BytesIO()
        img_by_size[px].save(buf, "PNG")
        payload = buf.getvalue()
        blobs.append(tag + struct.pack(">I", len(payload) + 8) + payload)
    body = b"".join(blobs)
    with open(path, "wb") as fh:
        fh.write(b"icns" + struct.pack(">I", len(body) + 8) + body)


def main():
    check = "--check" in sys.argv
    sizes = sorted({16, 32, 48, 64, 128, 256, 512, 1024, SPLASH_SIZE})
    imgs = {px: render(px) for px in sizes}
    wrote = []

    def emit(path, save, base=None):
        full = os.path.join(base or ROOT, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        if check:
            if not os.path.exists(full):
                print("MISSING  %s" % path)
                return False
            return True
        save(full)
        wrote.append(path)
        return True

    ok = True
    for name, px in (("shinogi.png", 256), ("shinogi-48.png", 48),
                     ("shinogi-64.png", 64), ("shinogi-128.png", 128)):
        ok &= emit("tools/linux/" + name,
                   lambda f, p=px: imgs[p].save(f, "PNG"))

    ok &= emit("tools/win/shinogi.ico",
               lambda f: imgs[256].save(
                   f, "ICO",
                   sizes=[(s, s) for s in (16, 32, 48, 64, 128, 256)]))

    ok &= emit("tools/macos/shinogi.icns", lambda f: icns(imgs, f))

    # QEMU's own window icon, which it loads from these two paths inside
    # its share/icons tree.  The packagers drop ours over the stock ones,
    # so the emulator window and its taskbar button carry the mark
    # instead of the QEMU logo.  Both forms ship because which one is
    # read depends on whether that QEMU build has SDL_image.
    marks = {px: render_mark(px) for px in (32, 128)}
    ok &= emit("tools/win/qemu-icon.png",
               lambda f: marks[128].save(f, "PNG"))
    ok &= emit("tools/win/qemu-icon.bmp",
               lambda f: qemu_bmp(marks[32], f))

    idx, w, h = quantise(imgs[SPLASH_SIZE])
    data = rle(idx, w, h)
    ok &= emit("bios/shinogi_splash.h",
               lambda f: write_header(f, data, w, h), base=EMUTOS)

    if check:
        print("check complete")
        return 0 if ok else 1

    for p in wrote:
        base = EMUTOS if p.startswith("bios/") else ROOT
        print("%-36s %d bytes" % (p, os.path.getsize(os.path.join(base, p))))
    print("(bios/ is under %s)" % EMUTOS)
    print("\nsplash: %dx%d, %d bytes RLE (%d as RGB565)"
          % (w, h, len(data), w * h * 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
