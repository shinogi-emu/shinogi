#!/usr/bin/env python3
"""Corrupt real directory replies and require the guest to reject each one."""
import argparse
import hashlib
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time


def receive(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise EOFError()
        data.extend(part)
    return bytes(data)


def reply(cookie=1, count=1, payload=None):
    if payload is None:
        payload = struct.pack("<IH", 1, 1) + b"x"
    return struct.pack("<HhII", 12 + len(payload), 0, cookie, count) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="shinogi-batch-guest-"))
    cases = {
        "zero-count": (18, reply(count=0), "invalid directory batch count"),
        "long-name": (18, reply(payload=struct.pack("<IH", 1, 256) + b"x"), "invalid directory batch entry"),
        "short-name": (18, reply(payload=struct.pack("<IH", 1, 8) + b"x"), "invalid directory batch entry"),
        "same-cookie": (18, reply(cookie=0, payload=struct.pack("<IH", 0, 1) + b"x"), "invalid directory batch entry"),
        "nul-name": (18, reply(payload=struct.pack("<IH", 1, 1) + b"\0"), "NUL in directory batch name"),
        "last-cookie": (18, reply(cookie=2), "inconsistent directory batch tail"),
        "trailing-byte": (18, reply(payload=struct.pack("<IH", 1, 1) + b"xy"), "inconsistent directory batch tail"),
        "capability-payload": (17, reply(payload=b"x"), "invalid capability reply"),
    }
    for name, (operation, corrupt, expected) in cases.items():
        out = root / name
        out.mkdir()
        drive = out / "drive"
        drive.mkdir()
        (drive / "HELLO.TXT").write_text("fixture\n")
        before = hashlib.sha256((drive / "HELLO.TXT").read_bytes()).digest()
        helper = subprocess.Popen([str(args.helper), "--root", str(drive), "--listen",
            str(out / "helper.sock"), "--ready-file", str(out / "ready"), "--once"],
            stdout=(out / "helper.log").open("wb"), stderr=subprocess.STDOUT)
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(out / "proxy.sock"))
        server.listen(1)
        applied = threading.Event()
        errors = []

        def proxy():
            try:
                client, unused = server.accept()
                with client, socket.socket(socket.AF_UNIX) as upstream:
                    upstream.connect(str(out / "helper.sock"))
                    while True:
                        header = receive(client, 16)
                        size, opcode = struct.unpack_from("<HH", header)
                        assert 16 <= size <= 4096
                        upstream.sendall(header + receive(client, size - 16))
                        answer = receive(upstream, 12)
                        length = struct.unpack_from("<H", answer)[0]
                        answer += receive(upstream, length - 12)
                        if opcode == operation and not applied.is_set():
                            answer = corrupt
                            applied.set()
                        client.sendall(answer)
            except (EOFError, ConnectionResetError, BrokenPipeError):
                pass
            except Exception as error:
                errors.append(repr(error))

        thread = threading.Thread(target=proxy, daemon=True)
        thread.start()
        qemu = None
        try:
            deadline = time.monotonic() + 5
            while not (out / "ready").exists() and time.monotonic() < deadline:
                assert helper.poll() is None
                time.sleep(.01)
            assert (out / "ready").exists()
            qemu = subprocess.Popen([str(args.qemu), "-M", "virt", "-cpu", "m68060", "-m", "128",
                "-kernel", str(args.firmware), "-append", "shinogi.selftest=1",
                "-device", "virtio-gpu-device",
                "-chardev", f"socket,id=fs,path={out}/proxy.sock,server=off",
                "-device", "virtio-serial-device",
                "-device", "virtserialport,chardev=fs,name=shinogi.hostfs",
                "-display", "none", "-serial", f"file:{out}/serial.log"],
                stdout=(out / "qemu.log").open("wb"), stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                serial = (out / "serial.log").read_text(errors="replace") if (out / "serial.log").exists() else ""
                if f"channel dead ({expected})" in serial:
                    break
                assert qemu.poll() is None
                time.sleep(.01)
            else:
                raise RuntimeError(f"{name}: guest did not reject the corrupt reply")
            assert applied.is_set() and not errors, (name, errors)
            assert hashlib.sha256((drive / "HELLO.TXT").read_bytes()).digest() == before
            print("Rejected:", name, flush=True)
        finally:
            if qemu is not None and qemu.poll() is None:
                qemu.terminate()
                qemu.wait(timeout=5)
            server.close()
            if helper.poll() is None:
                helper.terminate()
                helper.wait(timeout=5)
            thread.join(timeout=2)
    print("Guest rejected all corrupt directory/capability replies; artifacts:", root)


if __name__ == "__main__":
    main()
