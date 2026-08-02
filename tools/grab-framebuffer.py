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

# EmuTOS/TOS default 16-colour ST palette. Index 0 is white, 15 is black.
PALETTE = [
    (255,255,255), (255,0,0),   (0,255,0),   (255,255,0),
    (0,0,255),     (255,0,255), (0,255,255), (187,187,187),
    (119,119,119), (170,0,0),   (0,170,0),   (170,170,0),
    (0,0,170),     (170,0,170), (0,170,170), (0,0,0),
]

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
    time.sleep(2.0)
    print(s.recv(65536).decode(errors="replace")[-200:])
finally:
    qemu.kill()
    time.sleep(0.5)

data = open(RAW, "rb").read()
print(f"dumped {len(data)} bytes from 0x{FB:08x}")

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
