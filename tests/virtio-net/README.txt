Virtio-net regression checks
===========================

Queue ownership and bounds (no emulator required):

  python3 tests/virtio-net/check-driver.py /path/to/virtio_net.c

The driver C code is compiled with mocked kernel/MMIO boundaries. The
device deliberately withholds completions to prove that a full TX ring
cannot overwrite a buffer it still owns. It then returns an out-of-order
descriptor. Receive lengths and feature negotiation after close/open are
also checked. Use --expect-failure with the old source: it must report
the defects before a passing new source is evidence of a fix.

Real guest traffic:

  m68k-atari-mint-gcc -m68020-60 -O2 -Wall -Wextra -Werror \
      tests/virtio-net/netprobe.c -o /tmp/netprobe.ttp
  python3 tools/check-virtio-net.py --driver /path/to/virtio_net.xif \
      --probe /tmp/netprobe.ttp --elf /path/to/emutos-virt.elf

This boots a fresh copy of the shipping FreeMiNT tree on m68060. Local
host servers echo 48 UDP requests and send/receive 2 MiB with checked
contents. The sequence repeats after eth0 is brought down and up; the
route is restored too, so route removal cannot masquerade as a driver
failure. Host PCAP, guest logs, uploaded-data results and input hashes
are retained in the reported temporary folder. No Internet is needed.

The timings measure a local synthetic workload, not Internet bandwidth
or a comparison with another emulator. The old driver can pass the live
reopen check on QEMU, which retains its feature registers; the native
test independently requires the driver to perform a fresh negotiation.
