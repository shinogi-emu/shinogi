#!/usr/bin/env python3
"""Verify the bundled DHCP client and callback in a disposable 060 guest.

--client names the extracted original SpareMiNT sbin/dhclient binary.
The guest uses QEMU's local NAT DHCP server. No host adapter/bridge is changed.
The PCAP must contain DISCOVER/OFFER/REQUEST/ACK, and host files verify the
assigned address, subnet broadcast, DNS and same-router RENEW/REBIND routes.
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def dhcp_types(path):
    data = path.read_bytes()
    if data[:4] == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    elif data[:4] == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    else:
        raise ValueError("Unsupported PCAP header")
    result = []
    offset = 24
    while offset + 16 <= len(data):
        _, _, length, _ = struct.unpack_from(endian + "IIII", data, offset)
        offset += 16
        frame = data[offset:offset + length]
        offset += length
        if len(frame) != length:
            raise ValueError("Truncated PCAP frame")
        if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
            continue
        udp = 14 + (frame[14] & 15) * 4
        if struct.unpack_from(">HH", frame, udp) not in ((68, 67), (67, 68)):
            continue
        packet = frame[udp + 8:]
        if len(packet) < 240 or packet[236:240] != b"\x63\x82\x53\x63":
            continue
        option = 240
        while option < len(packet):
            kind = packet[option]
            option += 1
            if kind == 255:
                break
            if kind == 0:
                continue
            if option >= len(packet):
                raise ValueError("Truncated DHCP option")
            count = packet[option]
            option += 1
            if option + count > len(packet):
                raise ValueError("Truncated DHCP option value")
            if kind == 53 and count == 1:
                result.append(packet[option])
            option += count
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--callback", type=pathlib.Path, default=ROOT / "tools/guest/shndhcp.sh")
    parser.add_argument("--tree", type=pathlib.Path,
                        default=pathlib.Path.home() / "shinogi-build/mint-install-fix")
    parser.add_argument("--qemu", type=pathlib.Path,
                        default=pathlib.Path.home() / "shinogi-build/qemu-060/build-gui/qemu-system-m68k")
    parser.add_argument("--elf", type=pathlib.Path,
                        default=pathlib.Path.home() / "shinogi-build/emutos-060/emutos-virt.elf")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    subprocess.run(["make", "-C", str(ROOT / "tests/win-settings"), "test_network"], check=True)
    script = subprocess.check_output([str(ROOT / "tests/win-settings/test_network"), "--dhcp-script"]).decode()
    out = args.out or pathlib.Path(tempfile.mkdtemp(prefix="shinogi-dhcp-"))
    out.mkdir(exist_ok=True)
    tree = out / "drive"
    shutil.copytree(args.tree, tree)
    (tree / "AUTO/FVDI.PRG").rename(tree / "AUTO/FVDI.OFF")
    shutil.copyfile(pathlib.Path.home() / "git/freemint-rc/sys/arch/060sp/060sp.prg", tree / "AUTO/060SP.PRG")
    sysdir = tree / "MINT/1-19-CUR"
    shutil.copyfile(args.driver, sysdir / "VIRTIONE.XIF")
    shutil.copyfile(args.client, sysdir / "SYS-ROOT/BIN/DHCLIENT")
    shutil.copyfile(args.callback, sysdir / "SHNDHCP.SH")
    script += """
/bin/ifconfig eth0 > /c/DHIF.LOG
/bin/cat /ram/etc/resolv.conf > /c/DHDNS.LOG
export interface=eth0 new_ip_address=10.0.2.15 new_subnet_mask=255.255.255.0
export old_routers=10.0.2.2 new_routers=10.0.2.2 new_domain_name_servers=10.0.2.3
export reason=RENEW
/bin/bash /c/mint/1-19-cur/shndhcp.sh
/bin/route > /c/RENEW.LOG
export reason=REBIND new_domain_name_servers=
/bin/bash /c/mint/1-19-cur/shndhcp.sh
/bin/route > /c/REBIND.LOG
/bin/cat /ram/etc/resolv.conf > /c/NODNS.LOG
/bin/cat /c/SHNNET.IP > /c/LASTIP.LOG
export reason=STOP
/bin/bash /c/mint/1-19-cur/shndhcp.sh
echo DHCPCHECKEND > /c/DHDONE.LOG
"""
    (sysdir / "SHNNET.SH").write_text(script)
    cnf = sysdir / "MINT.CNF"
    text = cnf.read_text()
    text = "\n".join("#" + line if line.startswith("GEM=") else line for line in text.splitlines())
    old = ("exec u:/bin/ifconfig eth0 addr 10.0.2.15 netmask 255.255.255.0 up\n"
           "exec u:/bin/route add default eth0 gw 10.0.2.2")
    if text.count(old) != 1:
        raise ValueError("Fixture has unexpected networking commands")
    text = text.replace(old, "exec u:/bin/bash u:/c/mint/1-19-cur/shnnet.sh")
    cnf.write_text(text)
    ready, sock = out / "ready", out / "h.sock"
    helper = qemu = None
    with (out / "helper.log").open("wb") as helper_log, (out / "qemu.log").open("wb") as qemu_log:
        try:
            helper = subprocess.Popen([str(ROOT / "tools/hostfsd/shinogi-hostfsd"), "--root", str(tree),
                "--listen", str(sock), "--ready-file", str(ready), "--once"], stdout=helper_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 5
            while not ready.exists():
                if helper.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Hostfs helper did not become ready")
                time.sleep(0.01)
            qemu = subprocess.Popen([str(args.qemu), "-M", "virt", "-cpu", "m68060", "-m", "128",
                "-kernel", str(args.elf), "-netdev", "user,id=n0,ipv6=off", "-device", "virtio-net-device,netdev=n0",
                "-device", "virtio-gpu-device", "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
                "-object", f"filter-dump,id=dump0,netdev=n0,file={out / 'network.pcap'}",
                "-chardev", f"socket,id=hostfs,path={sock},server=off", "-device", "virtio-serial-device",
                "-device", "virtserialport,chardev=hostfs,name=shinogi.hostfs", "-display", "none",
                "-serial", f"file:{out / 'serial.log'}"], stdout=qemu_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + args.timeout
            while qemu.poll() is None and time.monotonic() < deadline:
                if (tree / "DHDONE.LOG").exists():
                    break
                time.sleep(0.1)
        finally:
            for process in (qemu, helper):
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
    print("Artifacts:", out)
    done = tree / "DHDONE.LOG"
    if not done.exists() or "DHCPCHECKEND" not in done.read_text():
        raise RuntimeError("Guest did not complete the DHCP test")
    assigned = (tree / "DHIF.LOG").read_text()
    assert "10.0.2.15" in assigned and "netmask 255.255.255.0" in assigned
    assert "broadcast 10.0.2.255" in assigned
    assert "nameserver 10.0.2.3" in (tree / "DHDNS.LOG").read_text()
    for name in ("RENEW.LOG", "REBIND.LOG"):
        routes = [line.split() for line in (tree / name).read_text().splitlines()]
        assert any(len(fields) >= 3 and fields[0] in ("default", "0.0.0.0") and
                   fields[1] == "10.0.2.2" and fields[-1] == "eth0" for fields in routes), name + " lost the default route"
    assert not (tree / "NODNS.LOG").read_text().strip(), "Omitted DNS retained a stale resolver"
    assert (tree / "LASTIP.LOG").read_text().strip() == "10.0.2.15", "Last LAN address was not reported"
    assert not (tree / "SHNNET.IP").read_text().strip(), "Stopped lease retained its reported address"
    kinds = dhcp_types(out / "network.pcap")
    expected = [1, 2, 3, 5]
    stage = 0
    for kind in kinds:
        if kind == expected[stage]:
            stage += 1
            if stage == len(expected):
                break
    assert stage == len(expected), "PCAP did not show DHCP DISCOVER/OFFER/REQUEST/ACK"
    print("DHCP PASS: DORA, subnet broadcast, DNS, same-router renewal/rebind routes and omitted DNS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
