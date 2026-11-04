#!/usr/bin/env python3
"""Boot a disposable guest, verify real network traffic, then down/up eth0.

Build tests/virtio-net/netprobe.c with m68k-atari-mint-gcc first. The host
UDP echo and TCP stream servers are local and never require Internet access.
PCAP and guest results are retained in a fresh output folder.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def verify_capture(path):
    data = path.read_bytes()
    if data[:4] != b"\xd4\xc3\xb2\xa1":
        raise RuntimeError("unexpected capture format")
    offset = 24
    requests, replies = [], []
    while offset + 16 <= len(data):
        _, _, size, _ = struct.unpack_from("<IIII", data, offset)
        offset += 16
        frame = data[offset:offset + size]
        offset += size
        if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
            continue
        udp_offset = 14 + (frame[14] & 15) * 4
        udp_size = struct.unpack_from("!H", frame, udp_offset + 4)[0]
        payload = frame[udp_offset + 8:udp_offset + udp_size]
        if len(payload) != 1024:
            continue
        if payload != bytes([payload[0]]) * 1024 or payload[0] >= 48:
            raise RuntimeError("host capture contains a corrupt probe packet")
        target = requests if frame[26:30] == b"\x0a\x00\x02\x0f" else replies
        target.append(payload[0])
    expected = list(range(48)) * 2
    if requests != expected or replies != expected:
        raise RuntimeError(f"host capture: {len(requests)} requests, {len(replies)} replies; expected 96 each")
    print("Host capture: 96 complete UDP requests and matching replies verified")


def serve_udp(sock, stop):
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            data, peer = sock.recvfrom(65536)
            sock.sendto(data, peer)
        except socket.timeout:
            pass


def serve_tcp(sock, stop, results):
    sock.settimeout(0.2)
    payload = bytes(range(256)) * 256
    while not stop.is_set():
        try:
            conn, _ = sock.accept()
        except socket.timeout:
            continue
        with conn:
            conn.settimeout(10)
            try:
                for _ in range(32):
                    conn.sendall(payload)
                received = 0
                valid = True
                while received < 2 * 1024 * 1024:
                    block = conn.recv(65536)
                    if not block:
                        break
                    shift = received % 256
                    valid &= block == (payload * 2)[shift:shift + len(block)]
                    received += len(block)
                valid &= received == 2 * 1024 * 1024
                results.append({"upload_bytes": received, "content_ok": valid})
                if valid:
                    conn.sendall(b"OKAY")
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--tree", type=pathlib.Path,
                        default=pathlib.Path.home() / "shinogi-build/mint-install-fix")
    parser.add_argument("--qemu", type=pathlib.Path,
                        default=pathlib.Path.home() / "shinogi-build/qemu-060/build-gui/qemu-system-m68k")
    parser.add_argument("--elf", type=pathlib.Path,
                        default=pathlib.Path.home() / "shinogi-build/emutos-060/emutos-virt.elf")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    out = args.out or pathlib.Path(tempfile.mkdtemp(prefix="shinogi-net-"))
    out.mkdir(exist_ok=True)
    (out / "inputs.json").write_text(json.dumps({
        "driver": str(args.driver),
        "driver_sha256": hashlib.sha256(args.driver.read_bytes()).hexdigest(),
        "firmware": str(args.elf),
        "firmware_sha256": hashlib.sha256(args.elf.read_bytes()).hexdigest(),
        "qemu": str(args.qemu),
    }, indent=2) + "\n")
    tree = out / "drive"
    shutil.copytree(args.tree, tree)
    # Keep the real kernel and IP stack; the test does not need a desktop.
    (tree / "AUTO/FVDI.PRG").rename(tree / "AUTO/FVDI.OFF")
    shutil.copyfile(pathlib.Path.home() / "git/freemint-rc/sys/arch/060sp/060sp.prg",
                    tree / "AUTO/060SP.PRG")
    shutil.copyfile(args.driver, tree / "MINT/1-19-CUR/VIRTIONE.XIF")
    shutil.copyfile(args.probe, tree / "NETPROBE.TTP")
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    udp.bind(("127.0.0.1", 0))
    tcp.bind(("127.0.0.1", 0))
    tcp.listen(2)
    stop = threading.Event()
    uploads = []
    workers = [threading.Thread(target=serve_udp, args=(udp, stop)),
               threading.Thread(target=serve_tcp, args=(tcp, stop, uploads))]
    for worker in workers:
        worker.start()
    ports = f"{udp.getsockname()[1]} {tcp.getsockname()[1]}"
    cnf = tree / "MINT/1-19-CUR/MINT.CNF"
    text = cnf.read_text()
    text = "\n".join("#" + line if line.startswith("GEM=") else line
                     for line in text.splitlines())
    text += (f"\nexec u:/c/netprobe.ttp C:\\NETPRE.LOG {ports}\n"
             "exec u:/bin/ifconfig eth0 down\n"
             "exec u:/bin/ifconfig eth0 addr 10.0.2.15 netmask 255.255.255.0 up\n"
             "exec u:/bin/route add default eth0 gw 10.0.2.2\n"
             f"exec u:/c/netprobe.ttp C:\\NETPOST.LOG {ports}\n")
    cnf.write_text(text)
    sock = out / "hostfs.sock"
    ready = out / "ready"
    helper_log = (out / "helper.log").open("wb")
    qemu_log = (out / "qemu.log").open("wb")
    helper = subprocess.Popen([str(ROOT / "tools/hostfsd/shinogi-hostfsd"),
                               "--root", str(tree), "--listen", str(sock),
                               "--ready-file", str(ready), "--once"],
                              stdout=helper_log, stderr=subprocess.STDOUT)
    qemu = None
    try:
        deadline = time.monotonic() + 5
        while not ready.exists():
            if helper.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("hostfs helper did not become ready")
            time.sleep(0.01)
        qemu = subprocess.Popen([
            str(args.qemu), "-M", "virt", "-cpu", "m68060", "-m", "128",
            "-kernel", str(args.elf), "-netdev", "user,id=n0,ipv6=off",
            "-device", "virtio-net-device,netdev=n0", "-device", "virtio-gpu-device",
            "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
            "-object", f"filter-dump,id=dump0,netdev=n0,file={out / 'network.pcap'}",
            "-chardev", f"socket,id=hostfs,path={sock},server=off",
            "-device", "virtio-serial-device", "-device",
            "virtserialport,chardev=hostfs,name=shinogi.hostfs",
            "-display", "none", "-serial", f"file:{out / 'serial.log'}"],
            stdout=qemu_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and qemu.poll() is None:
            post = tree / "NETPOST.LOG"
            if post.exists() and b"NETPROBE PASS" in post.read_bytes():
                break
            if post.exists() and b"NETPROBE FAIL" in post.read_bytes():
                break
            time.sleep(0.1)
    finally:
        for proc in [qemu, helper]:
            if proc and proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
        stop.set()
        for worker in workers:
            worker.join(timeout=11)
        udp.close()
        tcp.close()
        helper_log.close()
        qemu_log.close()
    passed = True
    for name in ["NETPRE.LOG", "NETPOST.LOG"]:
        file = tree / name
        result = file.read_text() if file.exists() else "NO COMPLETION LOG"
        print(name + ":\n" + result)
        passed &= "NETPROBE PASS" in result
        if "rx_errors=" in result:
            passed &= all(int(n) == 0 for n in re.findall(r"(?:rx_errors|tx_errors)=(\d+)", result))
    passed &= len(uploads) == 2 and all(item["content_ok"] for item in uploads)
    (out / "host-uploads.json").write_text(json.dumps(uploads, indent=2) + "\n")
    try:
        verify_capture(out / "network.pcap")
    except (OSError, RuntimeError) as error:
        print("FAIL", error)
        passed = False
    print("Artifacts:", out)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
