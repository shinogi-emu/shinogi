#!/usr/bin/env python3
"""Measure the guest 200 Hz tick by sampling _hz_200 (0x4ba) over the QEMU monitor."""
import socket, subprocess, sys, time, re

ELF = sys.argv[1]
SETTLE = 3.0
WINDOW = 10.0
PORT = 55731

qemu = subprocess.Popen([
    "qemu-system-m68k", "-M", "virt", "-m", "128", "-kernel", ELF,
    "-nographic", "-serial", "null",
    "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def sample(sock):
    sock.sendall(b"xp/1wx 0x4ba\n")
    time.sleep(0.4)
    data = sock.recv(65536).decode(errors="replace")
    m = re.search(r"0*4ba:\s+0x([0-9a-f]+)", data)
    return int(m.group(1), 16) if m else None

try:
    time.sleep(SETTLE)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    time.sleep(0.5)
    s.recv(65536)

    t0 = time.time(); v0 = sample(s)
    time.sleep(WINDOW)
    t1 = time.time(); v1 = sample(s)

    if v0 is None or v1 is None:
        print("FAILED. raw:"); s.sendall(b"xp/1wx 0x4ba\n"); time.sleep(0.5); print(repr(s.recv(65536)[:400])); sys.exit(1)

    elapsed = t1 - t0
    ticks = v1 - v0
    rate = ticks / elapsed
    print(f"_hz_200: {v0} -> {v1}")
    print(f"ticks={ticks} over {elapsed:.3f}s")
    print(f"measured rate = {rate:.2f} Hz   (target 200)")
    print(f"error = {(rate - 200) / 200 * 100:+.2f}%")
finally:
    qemu.kill()
