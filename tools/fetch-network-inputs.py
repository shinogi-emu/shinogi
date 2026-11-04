#!/usr/bin/env python3
"""Stage pinned, unmodified Windows TAP and FreeMiNT DHCP packages."""

import gzip
import hashlib
import io
import os
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import urllib.request
import zipfile

INPUTS = (
    ("SHINOGI_TAP_ARCHIVE", "tap-windows6-9.27.0-win10.zip",
     "https://github.com/OpenVPN/tap-windows6/releases/download/9.27.0/dist.win10.zip",
     "36e2609b7ceefedcb978ce5c48caf9e0e5af83423717c4e2e3c1d7ebca8f62a5"),
    ("SHINOGI_TAP_SOURCE", "tap-windows6-9.27.0.tar.gz",
     "https://github.com/OpenVPN/tap-windows6/archive/refs/tags/9.27.0.tar.gz",
     "5e807ab86740b654a2a63866b8749f582f16258d8e4dbcb6813be488ec59dde3"),
    ("SHINOGI_DHCLIENT_RPM", "dhclient-3.1.ESV-1.m68kmint.rpm",
     "https://freemint.github.io/sparemint/sparemint/RPMS/m68kmint/dhclient-3.1.ESV-1.m68kmint.rpm",
     "ec5bd5b127647913e2e43e426e429971ddb9013da17429474bbcbdc473afe1a8"),
    ("SHINOGI_DHCP_SOURCE_RPM", "dhcp-3.1.ESV-1.src.rpm",
     "https://freemint.github.io/sparemint/sparemint/SRPMS/dhcp-3.1.ESV-1.src.rpm",
     "518a7b7fa65ac85447cb7ff0ca62c8521f43a86261f3ef504e97ae132965dc16"),
)


def rpm_files(data):
    """Read the gzip/newc RPM payload without an rpm installation."""
    if data[:4] != b"\xed\xab\xee\xdb":
        raise ValueError("Not an RPM package")
    offset = 96
    for header in range(2):
        if data[offset:offset + 3] != b"\x8e\xad\xe8":
            raise ValueError("Invalid RPM header")
        count, size = struct.unpack_from(">II", data, offset + 8)
        offset += 16 + count * 16 + size
        if offset > len(data):
            raise ValueError("Truncated RPM header")
        if header == 0:
            offset = (offset + 7) & ~7
    payload = gzip.decompress(data[offset:])
    result = {}
    offset = 0
    while payload[offset:offset + 6] in (b"070701", b"070702"):
        fields = [int(payload[offset + 6 + i * 8:offset + 14 + i * 8], 16)
                  for i in range(13)]
        length, namesize = fields[6], fields[11]
        offset += 110
        if not namesize or offset + namesize > len(payload):
            raise ValueError("Truncated RPM filename")
        name = payload[offset:offset + namesize - 1].decode("ascii")
        offset = (offset + namesize + 3) & ~3
        if offset + length > len(payload):
            raise ValueError("Truncated RPM file")
        content = payload[offset:offset + length]
        offset = (offset + length + 3) & ~3
        if name == "TRAILER!!!":
            return result
        name = name.removeprefix("./")
        if name.startswith("/") or ".." in Path(name).parts:
            raise ValueError("Unsafe RPM filename")
        if length:
            result[name] = content
    raise ValueError("RPM archive has no completion trailer")


def stage(bundle):
    root = bundle / "network"
    source = root / "source"
    driver = root / "tap"
    source.mkdir(parents=True, exist_ok=True)
    driver.mkdir(parents=True, exist_ok=True)
    paths = {}
    provenance = ["Shinogi network components", "==========================", ""]
    for variable, name, url, digest in INPUTS:
        target = source / name
        local = os.environ.get(variable)
        if local:
            shutil.copyfile(local, target)
        else:
            with urllib.request.urlopen(url, timeout=60) as response, target.open("wb") as output:
                shutil.copyfileobj(response, output)
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Checksum mismatch for {name}")
        paths[variable] = target
        provenance.extend((name, url, "SHA256 " + digest, ""))
    with zipfile.ZipFile(paths["SHINOGI_TAP_ARCHIVE"]) as archive:
        for name in ("devcon.exe", "OemVista.inf", "tap0901.cat", "tap0901.sys"):
            (driver / name).write_bytes(archive.read("dist.win10/amd64/" + name))
    shutil.copyfile(Path(__file__).resolve().parent / "win/devcon-license.txt",
                    driver / "DEVCON-LICENSE.txt")
    with tarfile.open(paths["SHINOGI_TAP_SOURCE"], "r:gz") as archive:
        for name in ("COPYING", "COPYRIGHT.GPL", "COPYRIGHT.MIT"):
            member = archive.extractfile("tap-windows6-9.27.0/" + name)
            if member is None:
                raise ValueError("TAP source is missing its license")
            (driver / name).write_bytes(member.read())
    client = rpm_files(paths["SHINOGI_DHCLIENT_RPM"].read_bytes())["sbin/dhclient"]
    (bundle / "DHCLIENT").write_bytes(client)
    guest_bin = bundle / "drive-c/MINT/1-19-CUR/SYS-ROOT/BIN"
    guest_bin.mkdir(parents=True, exist_ok=True)
    (guest_bin / "DHCLIENT").write_bytes(client)
    sources = rpm_files(paths["SHINOGI_DHCP_SOURCE_RPM"].read_bytes())
    with tarfile.open(fileobj=io.BytesIO(sources["dhcp-3.1.ESV.tar.gz"]), mode="r:gz") as archive:
        member = archive.extractfile("dhcp-3.1.ESV/LICENSE")
        if member is None:
            raise ValueError("DHCP source is missing its license")
        (root / "DHCP-LICENSE.txt").write_bytes(member.read())
    provenance.extend((
        "The TAP SYS/INF/CAT and devcon are unmodified official x64 package files.",
        "Windows verifies the package signature during explicit LAN setup.",
        "TAP SYS/INF/CAT licensing: GPLv2 with the upstream WDK exception; see tap/COPYING.",
        "devcon.exe is a separate Microsoft sample under MS-PL; see tap/DEVCON-LICENSE.txt.",
        "devcon source: https://github.com/OpenVPN/Windows-driver-samples/tree/master/setup/devcon",
        "devcon binary SHA256: " + hashlib.sha256((driver / "devcon.exe").read_bytes()).hexdigest(),
        "DHCP is the existing SpareMiNT ISC client; its source RPM includes the port patch.",
        "DHCP licensing: permissive ISC; see DHCP-LICENSE.txt.",
        "Matching original source packages are included in source/.",
    ))
    (root / "PROVENANCE.txt").write_text("\n".join(provenance) + "\n", encoding="ascii")
    print("Network inputs: pinned TAP driver, corresponding sources and FreeMiNT DHCP client staged")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: fetch-network-inputs.py BUNDLE")
    try:
        stage(Path(sys.argv[1]))
    except (OSError, ValueError, KeyError, struct.error) as error:
        raise SystemExit(str(error))
