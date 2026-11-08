#!/usr/bin/env python3
"""Time completed, checksum-checked guest work on QEMU and WinUAE JIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pty
import selectors
import signal
import statistics
import subprocess
import time


def sample(engine, executable, case, images, out, core, timeout, softfloat,
           qemu_cpu, qemu_machine, winuae_cache):
    log = out.with_suffix(".log")
    slave = None
    if engine.startswith("qemu"):
        cmd = [str(executable), "-M", qemu_machine, "-cpu", qemu_cpu, "-m", "16",
               "-kernel", str(images/(case+".elf")), "-display", "none",
               "-monitor", "none", "-serial", "stdio", "-no-reboot"]
    else:
        master, slave = pty.openpty()
        prefs = {
            "use_gui": "no", "kickstart_rom_file": str(images/(case+".rom")),
            "nr_floppies": "0", "chipset": "ecs", "chipmem_size": "2",
            "fastmem_size": "8", "cpu_model": "68060", "fpu_model": "68060",
            "cpu_24bit_addressing": "false", "cpu_compatible": "false",
            "cycle_exact": "false", "cpu_speed": "max", "cachesize": str(winuae_cache),
            "compfpu": "true",
            "fpu_softfloat": str(softfloat).lower(), "sound_output": "none",
            "serial_on_demand": "false", "serial_hardware_ctsrts": "false",
            "unix.serial_port": os.ttyname(slave),
        }
        cmd = [str(executable)]
        for key, value in prefs.items():
            cmd += ["-s", key+"="+value]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    started = ended = None
    markers = []
    pending = b""
    with log.open("wb") as errors:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE if engine.startswith("qemu") else errors,
                                stderr=errors, cwd=out.parent, env=env,
                                preexec_fn=lambda: os.sched_setaffinity(0, {core}))
        fd = proc.stdout.fileno() if engine.startswith("qemu") else master
        os.set_blocking(fd, False)
        selector = selectors.DefaultSelector()
        selector.register(fd, selectors.EVENT_READ)
        deadline = time.monotonic()+timeout
        try:
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError(f"{engine} exited before completion; see {log}")
                for _, _ in selector.select(.1):
                    try:
                        chunk = os.read(fd, 4096)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise RuntimeError(f"{engine} closed its console; see {log}")
                    now = time.perf_counter_ns()
                    pending += chunk
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        line = line.strip(b"\r")
                        markers.append(line.decode("ascii", "replace"))
                        if line == b"B" and markers == ["B"]:
                            started = now
                        elif line == b"E" and markers == ["B", "E"]:
                            ended = now
                        elif line == b"P" and markers == ["B", "E", "P"]:
                            if ended <= started:
                                raise RuntimeError("timer resolution insufficient; increase workload scale")
                            return {"engine": engine, "case": case,
                                    "seconds": (ended-started)/1e9, "markers": markers,
                                    "command": cmd, "log": str(log)}
                        else:
                            raise RuntimeError(f"invalid guest completion {markers}; see {log}")
            raise RuntimeError(f"{engine} timed out; markers={markers}; see {log}")
        finally:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            selector.close()
            if proc.stdout:
                proc.stdout.close()
            if slave is not None:
                os.close(slave)
                os.close(master)
            if engine == "winuae" and markers == ["B", "E", "P"]:
                errors.flush()
                text = log.read_text(errors="replace")
                if winuae_cache and ("translation cache" not in text or "JIT=0" in text):
                    raise RuntimeError(f"WinUAE JIT was not confirmed; see {log}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", type=Path)
    p.add_argument("--reference-qemu", type=Path)
    p.add_argument("--qemu-cpu", default="m68060")
    p.add_argument("--qemu-machine", default="virt")
    p.add_argument("--winuae", type=Path)
    p.add_argument("--images", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cases", nargs="+")
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--core", type=int, default=max(os.sched_getaffinity(0)))
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument("--winuae-softfloat", action="store_true")
    p.add_argument("--winuae-cache", type=int, default=16384)
    p.add_argument("--min-seconds", type=float, default=.1)
    a = p.parse_args()
    engines = {k: v.resolve() for k, v in (("qemu", a.qemu), ("qemu-reference", a.reference_qemu), ("winuae", a.winuae)) if v}
    if not engines or a.runs < 1 or a.core not in os.sched_getaffinity(0):
        p.error("provide an emulator, a positive run count and an available CPU core")
    a.images = a.images.resolve()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((a.images/"manifest.json").read_text())
    cases = a.cases or list(manifest)
    if any(case not in manifest for case in cases):
        p.error("unknown workload")
    for case in cases:
        for ext in ("elf", "rom"):
            if hashlib.sha256((a.images/(case+"."+ext)).read_bytes()).hexdigest() != manifest[case][ext+"_sha256"]:
                p.error(f"{case}.{ext} differs from the manifest; rebuild the images")
    result = {"complete": False, "manifest": manifest, "core": a.core,
              "host": dict(zip(("sysname", "nodename", "release", "version", "machine"), os.uname())),
              "winuae_softfloat": a.winuae_softfloat,
              "executables": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                              for name, path in engines.items()}, "samples": []}
    for case in cases:
        for run in range(a.runs):
            order = list(engines) if run % 2 == 0 else list(reversed(engines))
            for engine in order:
                value = sample(engine, engines[engine], case, a.images,
                               a.out/f"{case}-{run}-{engine}", a.core, a.timeout,
                               a.winuae_softfloat, a.qemu_cpu, a.qemu_machine,
                               a.winuae_cache)
                if value["seconds"] < a.min_seconds:
                    raise RuntimeError(f"sample too short ({value['seconds']:.6f}s); increase workload scale")
                result["samples"].append(value)
                (a.out/"results.json").write_text(json.dumps(result, indent=2)+"\n")
                print(f"{case} {engine} {run+1}: {value['seconds']:.6f} s", flush=True)
    result["complete"] = True
    (a.out/"results.json").write_text(json.dumps(result, indent=2)+"\n")
    for case in cases:
        for engine in engines:
            values = [s["seconds"] for s in result["samples"] if s["case"] == case and s["engine"] == engine]
            print(f"{case} {engine}: median {statistics.median(values):.6f} s, range {min(values):.6f}..{max(values):.6f}")


if __name__ == "__main__":
    main()
