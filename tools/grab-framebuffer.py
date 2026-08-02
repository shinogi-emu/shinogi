#!/usr/bin/env python3
"""Dump the guest framebuffer over the QEMU monitor and decode it to PNG.

The VDI is rendering into ST-RAM long before virtio-gpu exists, so the
desktop can be inspected today. Format is Atari 4-plane interleaved:
each group of 16 pixels is 4 consecutive big-endian words, one per
bitplane, MSB = leftmost pixel.
"""
import socket, subprocess, sys, time, os
from PIL import Image

ELF = sys.argv[1]
OUT = sys.argv[2]
FB = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00df8000
W, H, PLANES = 320, 200, 4
SETTLE = 8.0
PORT = 55733
RAW = "/tmp/shinogi-fb.bin"

# The palette is read from the guest rather than assumed. The VDI keeps
# REQ_COL[16][3] (requested colour per pen, 0..1000 per component) and
# REV_MAP_COL[16] (hardware register -> VDI pen). Bitplane values are
# hardware register indices, so both tables are needed. Addresses come
# from emutos.map and move on every rebuild.
def sym(mapfile, name):
    """Look a symbol up in emutos.map -- these addresses move every rebuild."""
    import re
    pat = re.compile(r"0x([0-9a-f]+)\s+_" + name + r"$")
    for line in open(mapfile):
        m = pat.search(line.strip())
        if m:
            return int(m.group(1), 16)
    raise SystemExit(f"symbol {name} not found in {mapfile}")

MAP = os.path.join(os.path.dirname(os.path.abspath(ELF)), "emutos.map")
REQ_COL_ADDR = sym(MAP, "REQ_COL")
REV_MAP_ADDR = sym(MAP, "REV_MAP_COL")
print(f"REQ_COL=0x{REQ_COL_ADDR:08x} REV_MAP_COL=0x{REV_MAP_ADDR:08x}")

if os.path.exists(RAW):
    os.unlink(RAW)

qemu = subprocess.Popen([
    "qemu-system-m68k", "-M", "virt", "-m", "128", "-kernel", ELF,
    "-nographic", "-serial", "null",
    "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    time.sleep(SETTLE)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    time.sleep(0.5); s.recv(65536)
    s.sendall(f"memsave 0x{FB:x} {W*H*PLANES//8} \"{RAW}\"\n".encode())
    time.sleep(2.0); s.recv(65536)
    s.sendall(f"memsave 0x{REQ_COL_ADDR:x} 96 \"{RAW}.req\"\n".encode())
    time.sleep(1.0); s.recv(65536)
    s.sendall(f"memsave 0x{REV_MAP_ADDR:x} 32 \"{RAW}.rev\"\n".encode())
    time.sleep(1.0); s.recv(65536)
finally:
    qemu.kill()
    time.sleep(0.5)

data = open(RAW, "rb").read()
print(f"dumped {len(data)} bytes from 0x{FB:08x}")

def be16(b, i):
    return (b[2*i] << 8) | b[2*i+1]

req = open(RAW + ".req", "rb").read()
rev = open(RAW + ".rev", "rb").read()

# VDI components are 0..1000; scale to 0..255.
pen_rgb = []
for pen in range(16):
    r, g, b = (be16(req, pen*3 + c) for c in range(3))
    pen_rgb.append(tuple(min(255, (v * 255 + 500) // 1000) for v in (r, g, b)))

PALETTE = []
for hw in range(16):
    pen = be16(rev, hw)
    PALETTE.append(pen_rgb[pen] if pen < 16 else (255, 0, 255))
print("palette from guest:", PALETTE)

img = Image.new("RGB", (W, H))
px = img.load()
stride = W * PLANES // 8          # 160 bytes per scanline
for y in range(H):
    row = y * stride
    for gx in range(W // 16):
        off = row + gx * 8
        planes = [(data[off+2*p] << 8) | data[off+2*p+1] for p in range(PLANES)]
        for bit in range(16):
            shift = 15 - bit
            idx = 0
            for p in range(PLANES):
                idx |= ((planes[p] >> shift) & 1) << p
            px[gx*16 + bit, y] = PALETTE[idx]

img = img.resize((W*2, H*2), Image.NEAREST)
img.save(OUT)
print(f"wrote {OUT}")

hist = {}
for y in range(H):
    for x in range(W):
        hist[px[x, y]] = hist.get(px[x, y], 0) + 1
print("top colours:", sorted(hist.items(), key=lambda kv: -kv[1])[:4])
