#!/usr/bin/env python3
"""Check optional directory pages against v1 names/cookies and mutations."""
from pathlib import Path
import argparse
import socket
import struct
import subprocess
import tempfile
import time


def receive(connection, size):
    chunks = bytearray()
    while len(chunks) < size:
        part = connection.recv(size - len(chunks))
        assert part, "truncated reply"
        chunks.extend(part)
    return bytes(chunks)


def rpc(connection, opcode, a=0, b=0, data=b""):
    connection.sendall(struct.pack("<HHIII", 16 + len(data), opcode, a, b, 0) + data)
    header = receive(connection, 12)
    length, status, a, b = struct.unpack("<HhII", header)
    assert 12 <= length <= 4096
    return status, a, b, receive(connection, length - 12)


def listing(connection, handle, batched):
    cookie, result, calls = 0, [], 0
    while True:
        status, last, count, payload = rpc(connection, 18 if batched else 8, handle, cookie)
        calls += 1
        if status == -49:
            break
        assert status == 0
        if not batched:
            assert last != cookie and payload
            result.append((last, payload))
            cookie = last
            continue
        offset = 0
        assert 1 <= count <= 16
        for unused in range(count):
            next_cookie, size = struct.unpack_from("<IH", payload, offset)
            offset += 6
            assert next_cookie != cookie and 1 <= size <= 255
            name = payload[offset:offset + size]
            assert len(name) == size and b"\0" not in name
            result.append((next_cookie, name))
            offset += size
            cookie = next_cookie
        assert offset == len(payload) and cookie == last
    return result, calls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    args = parser.parse_args()
    out = Path(tempfile.mkdtemp(prefix="shinogi-batch-test-"))
    root = out / "drive"
    root.mkdir()
    for n in range(170):
        (root / f"entry{n:04d}.txt").write_text(str(n))
    for n in range(20):
        (root / (f"long{n:03d}-" + "x" * 247)).write_text("long name")
    (root / "EightCharactersLong.txt").write_text("alias 1")
    (root / "EightCharOther.txt").write_text("alias 2")
    helper = subprocess.Popen([str(args.helper), "--root", str(root), "--listen",
        str(out / "socket"), "--ready-file", str(out / "ready"), "--once"],
        stdout=(out / "helper.log").open("wb"), stderr=subprocess.STDOUT)
    try:
        for unused in range(200):
            if (out / "ready").exists():
                break
            assert helper.poll() is None
            time.sleep(.01)
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(str(out / "socket"))
            assert rpc(connection, 1, 1)[0] == 0
            assert rpc(connection, 17) == (0, 1, 0, b"")
            legacy_handle = rpc(connection, 7)[1]
            batch_handle = rpc(connection, 7)[1]
            # Both snapshots retain the names/cookies, and skip deletion.
            (root / "entry0004.txt").rename(root / "changed.txt")
            old, old_calls = listing(connection, legacy_handle, False)
            new, new_calls = listing(connection, batch_handle, True)
            assert old == new and len(new) == 191
            assert new_calls < old_calls / 8
            assert rpc(connection, 18, batch_handle, 0xffffffff)[0] == -49
            assert rpc(connection, 18, 0)[0] == -37
            assert rpc(connection, 19)[0] == -46
            assert rpc(connection, 17)[0] == 0
            for handle in [legacy_handle, batch_handle]:
                assert rpc(connection, 9, handle)[0] == 0
                assert rpc(connection, 18, handle)[0] == -37
        assert helper.wait(timeout=5) == 0
        print(f"Directory batches: {len(new)} exact names/cookies, {old_calls} -> {new_calls} calls; mutation, bounds and recovery passed")
        print("Artifacts:", out)
    finally:
        if helper.poll() is None:
            helper.terminate()
            helper.wait(timeout=5)


if __name__ == "__main__":
    main()
