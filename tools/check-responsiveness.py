#!/usr/bin/env python3
"""Compare actual 060 boot, idle CPU, flushes and input-to-frame latency.

This measures the guest path through QMP with a headless display. It does
not include Windows SDL event polling or the laptop's screen/compositor.
All drive changes and screenshots stay in fresh disposable copies.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import socket
import statistics
import subprocess
import time


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile("rwb", buffering=0)
        assert "QMP" in json.loads(self.stream.readline())
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        message = {"execute": command}
        if arguments is not None:
            message["arguments"] = arguments
        self.stream.write((json.dumps(message) + "\n").encode())
        while True:
            reply = json.loads(self.stream.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(reply)


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def image(monitor, destination):
    monitor.call("screendump", {"filename": str(destination), "device": "vgpu"})
    header, dimensions, maximum, pixels = destination.read_bytes().split(b"\n", 3)
    assert header == b"P6" and maximum == b"255"
    width, height = map(int, dimensions.split())
    assert (width, height) == (1024, 768)
    return pixels


def patch(pixels, position):
    x, y = position
    return b"".join(pixels[(row * 1024 + x) * 3:(row * 1024 + x + 20) * 3]
                    for row in range(y, y + 20))


def move(monitor, position):
    x, y = position
    monitor.call("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / 1023)}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / 767)}}]})


def measure(args, firmware, name, selftests=False):
    out = args.out / name
    out.mkdir()
    tree = out / "drive"
    shutil.copytree(args.tree, tree)
    for log in tree.rglob("XA_BOOT.LOG"):
        log.rename(log.with_suffix(".OLD"))
    helper = subprocess.Popen([str(args.helper), "--root", str(tree), "--listen",
        str(out / "fs.sock"), "--ready-file", str(out / "ready"), "--once"],
        stdout=(out / "helper.log").open("wb"), stderr=subprocess.STDOUT)
    qemu = None
    try:
        deadline = time.monotonic() + 5
        while not (out / "ready").exists() and time.monotonic() < deadline:
            assert helper.poll() is None
            time.sleep(.01)
        assert (out / "ready").exists()
        command = [str(args.qemu), "-M", "virt", "-cpu", "m68060", "-m", "128",
            "-kernel", str(firmware), "-netdev", "user,id=n0,ipv6=off",
            "-device", "virtio-net-device,netdev=n0",
            "-device", "virtio-gpu-device,id=vgpu,xres=1024,yres=768",
            "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
            "-chardev", f"socket,id=fs,path={out}/fs.sock,server=off",
            "-device", "virtio-serial-device",
            "-device", "virtserialport,chardev=fs,name=shinogi.hostfs",
            "-display", "none", "-serial", f"file:{out}/serial.log",
            "-qmp", f"unix:{out}/qmp,server=on,wait=off",
            "-trace", f"enable=virtio_gpu_cmd_res_flush,file={out}/flush.log"]
        if selftests:
            command += ["-append", "shinogi.selftest=1"]
        started = time.monotonic()
        qemu = subprocess.Popen(command, stdout=(out / "qemu.log").open("wb"),
                                stderr=subprocess.STDOUT)
        deadline = started + 45
        shell_log = tree / "MINT/1-19-CUR/XAAES/XA_BOOT.LOG"
        while time.monotonic() < deadline:
            assert qemu.poll() is None, (out / "qemu.log").read_text()
            if shell_log.exists() and "Launch shell" in shell_log.read_text(errors="replace"):
                break
            time.sleep(.005)
        else:
            raise RuntimeError(f"{name}: desktop boot timed out")
        result = {"desktop_seconds": time.monotonic() - started,
                  "firmware_sha256": hashlib.sha256(firmware.read_bytes()).hexdigest()}
        monitor = Monitor(out / "qmp")
        time.sleep(.6)
        before_cpu, before_time = cpu_seconds(qemu.pid), time.monotonic()
        idle_start = (out / "flush.log").stat().st_size
        time.sleep(2)
        result["idle_cpu_percent"] = 100 * (cpu_seconds(qemu.pid) - before_cpu) / (time.monotonic() - before_time)
        result["idle_flushes"] = (out / "flush.log").read_bytes()[idle_start:].count(b"virtio_gpu_cmd_res_flush")
        positions = [(300, 300), (700, 500)]
        move(monitor, positions[1])
        time.sleep(.25)
        pixels = image(monitor, out / "desktop.ppm")
        latencies = []
        randomizer = random.Random(17)
        for number in range(16):
            position = positions[number % 2]
            previous = patch(pixels, position)
            time.sleep(randomizer.uniform(.003, .023))
            start = time.monotonic()
            move(monitor, position)
            while time.monotonic() - start < .5:
                pixels = image(monitor, out / "desktop.ppm")
                if patch(pixels, position) != previous:
                    break
                time.sleep(.003)
            else:
                raise RuntimeError(f"{name}: cursor never appeared at {position}")
            latencies.append((time.monotonic() - start) * 1000)
        result["input_frame_ms"] = latencies
        result["input_frame_median_ms"] = statistics.median(latencies)
        result["input_frame_max_ms"] = max(latencies)
        trace_start = (out / "flush.log").stat().st_size
        start = time.monotonic()
        while time.monotonic() - start < 1.2:
            t = time.monotonic() - start
            move(monitor, (300 + int(t * 200) % 300, 350 + int(t * 80) % 100))
            time.sleep(.004)
        elapsed = time.monotonic() - start
        traces = (out / "flush.log").read_bytes()[trace_start:]
        result["motion_flush_hz"] = traces.count(b"virtio_gpu_cmd_res_flush") / elapsed
        rectangles = [tuple(map(int, match)) for match in
                      re.findall(rb"w (\d+), h (\d+), x (\d+), y (\d+)", traces)]
        result["motion_mean_flush_pixels"] = statistics.mean(w * h for w, h, x, y in rectangles)
        result["serial_bytes"] = (out / "serial.log").stat().st_size
        monitor.call("quit")
        assert qemu.wait(timeout=10) == 0
        helper.wait(timeout=5)
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(name, json.dumps({k: v for k, v in result.items() if k != "input_frame_ms"}), flush=True)
        return result
    finally:
        for process in [qemu, helper]:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(exist_ok=False)
    baseline = measure(args, args.baseline, "baseline")
    candidate = measure(args, args.candidate, "candidate")
    assert baseline["motion_flush_hz"] < 15, "Baseline is not the divided-rate firmware"
    assert candidate["motion_flush_hz"] > 35, "Candidate missed the responsive-frame gate"
    assert candidate["input_frame_median_ms"] < baseline["input_frame_median_ms"] / 2
    assert candidate["motion_mean_flush_pixels"] < 1024 * 768 / 4
    assert candidate["idle_cpu_percent"] < baseline["idle_cpu_percent"] * 1.25
    print("Responsiveness checks passed; artifacts:", args.out)


if __name__ == "__main__":
    main()
