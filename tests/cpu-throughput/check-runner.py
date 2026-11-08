#!/usr/bin/env python3
"""Require a completed workload with a wrong checksum to be rejected."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--qemu", required=True)
a = p.parse_args()
root = Path(__file__).resolve().parent
work = Path(tempfile.mkdtemp(prefix="cpu-throughput-check-"))
images = work/"images"
subprocess.run([sys.executable, str(root/"build.py"), "--out", str(images), "--scale", ".01"], check=True)
manifest = json.loads((images/"manifest.json").read_text())
image = images/"integer.elf"
data = bytearray(image.read_bytes())
needle = b"\x2a\x3c" + struct.pack(">I", manifest["integer"]["expected"])
assert data.count(needle) == 1
index = data.index(needle)
data[index+5] ^= 1
image.write_bytes(data)
# Accept the deliberately changed artifact so the guest result is tested.
manifest["integer"]["elf_sha256"] = hashlib.sha256(data).hexdigest()
(images/"manifest.json").write_text(json.dumps(manifest))
result = subprocess.run([sys.executable, str(root/"run.py"), "--qemu", a.qemu,
                         "--images", str(images), "--out", str(work/"run"),
                         "--cases", "integer", "--runs", "1", "--min-seconds", "0"],
                        text=True, capture_output=True, timeout=30)
assert result.returncode != 0
assert "invalid guest completion ['B', 'E', 'F']" in result.stderr, result.stderr
print("PASS: completed workload with a wrong result was rejected", work)
