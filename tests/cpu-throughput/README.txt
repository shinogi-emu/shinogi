CPU throughput: QEMU and WinUAE JIT
=================================

This Linux harness runs the same 68060 payload bytes at the same guest
addresses in QEMU and WinUAE. It measures fixed guest work between console
markers with host monotonic time, after warming the actual loop. Boot time,
initialization, output validation and shutdown are outside the timed region.
Each guest must reach both timing markers, consume its loop counter, and
pass its result checks. Memory cases also validate the destination after
clearing it between warmup and measurement. A missing or incorrect result
is an error. Complete result files explicitly contain complete=true.

The synthetic cases cover add/sub arithmetic, alternating branches, shifts,
copy-plus-checksum memory traffic, and FADD/FSUB/FMUL/FSQRT on exact native
values. The memory-offset control changes only destination alignment by
4 KiB. Memory-flush executes PFLUSHA before every 256 KiB copy, to exercise
soft-TLB resizing across guest flushes. Memory-copy copies 1 GiB with a scalar
MOVE.L loop; memory-movem copies the same amount eight longs at a time with
MOVEM.L. Both validate the entire destination after measurement. These are focused throughput tests,
not application benchmarks or a complete ISA/FPU conformance suite.

Build and run
-------------

Requires Python 3, m68k-atari-mintelf tools and native Linux emulators.

  python3 tests/cpu-throughput/build.py --out /tmp/cpu-images
  python3 tests/cpu-throughput/run.py \
      --qemu /absolute/path/to/candidate/qemu-system-m68k \
      --reference-qemu /absolute/path/to/baseline/qemu-system-m68k \
      --winuae /absolute/path/to/winuae \
      --images /tmp/cpu-images --out /tmp/cpu-results --runs 5

Each emulator argument is optional; at least one is required. All child
threads are pinned to the same logical CPU (last available by default;
override with --core). Arm order reverses on alternate repetitions. Use
--cases to select workloads. Increase build.py --scale if any timed sample
is shorter than the default 0.1-second minimum. Smaller minimums are useful
for bringup only. --out must be a new directory to avoid stale results.
The manifest binds ELF/ROM hashes and records iterations, expected results
and the shared payload hash. Exact executable hashes and commands are saved.

The reset ROM is generated here; no commercial ROM, operating system or
disk image is required. Its bootstrap configures Zorro-II fast RAM, copies
the shared payload there and uses the serial port for markers. QEMU loads
that payload from ELF. The guest explicitly enables its 060 FPU and cache
before warmup. The bootstrap uses 060 PCR/CACR semantics; select compatible
models when overriding --qemu-cpu or --qemu-machine. For the 040, build a
separate image directory with build.py --bootstrap-cpu m68040 and run it with
run.py --qemu-cpu m68040. This omits the 060-only PCR setup; the timed work
and checks are identical.

Verify that wrong results cannot pass:

  python3 tests/cpu-throughput/check-runner.py \
      --qemu /absolute/path/to/qemu-system-m68k

This runs a real workload with an intentionally wrong expected checksum
and requires the runner to reject its B/E/F completion sequence.

WinUAE comparison build
-----------------------

The initial reference is tonioni/WinUAE commit
d82bc2549f27943d11dc59482d5388daf1c0f337 (2026-11-08), built with its Unix
x86-64 JIT. Use a separate checkout. winuae-headless.patch supplies two
missing CLI logging globals when optional host backends are absent; it
does not change the CPU, JIT, memory-access or FPU code.

  git -C /path/to/separate/WinUAE checkout --detach d82bc2549f27943d11dc59482d5388daf1c0f337
  git -C /path/to/separate/WinUAE apply /absolute/path/to/winuae-headless.patch
  cmake -S /path/to/separate/WinUAE -B /tmp/winuae-throughput -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DWINUAE_UNIX_WITH_JIT=ON \
      -DWINUAE_UNIX_WITH_QT_UI=OFF -DWINUAE_UNIX_WITH_LUA=OFF \
      -DWINUAE_UNIX_WITH_ARCHIVES=OFF -DWINUAE_UNIX_WITH_CHD=OFF \
      -DWINUAE_UNIX_WITH_PRINTER_EMULATION=OFF \
      -DWINUAE_UNIX_WITH_PPC_QEMU=OFF -DWINUAE_UNIX_BUILD_QEMU_UAE_PLUGIN=OFF \
      -DWINUAE_UNIX_WITH_FLOPPYBRIDGE=OFF \
      -DWINUAE_UNIX_BUILD_FLOPPYBRIDGE_PLUGIN=OFF \
      -DWINUAE_UNIX_BUNDLE_DRIVE_SOUNDS=OFF -DWINUAE_UNIX_WITH_CAPS=OFF \
      -DWINUAE_UNIX_WITH_MIDIEMU=OFF
  cmake --build /tmp/winuae-throughput --target winuae_unix -j 8

The runner selects 68060, unlimited CPU speed, a 16 MiB JIT cache, JIT FPU,
no cycle-exact mode, no sound, and dummy video/audio. It checks the log for
JIT availability. --winuae-cache 0 is an interpreter control; it must not be
used as the WinUAE JIT baseline. --winuae-softfloat selects its separate
software FPU mode. The default reference uses WinUAE's native FPU; the
arithmetic case deliberately stays on exact finite values and does not
establish equivalent exception, precision or rounding behavior generally.

The measured Unix port selects indirect JIT memory accesses under
NATMEM_NO_JIT_DIRECT, even when the backing RAM is directly mapped. Results
therefore describe this pinned Linux WinUAE JIT configuration. They do not
establish parity with native Windows WinUAE using JIT direct memory.
Physical display/compositor and host input latency are separate checks.

Correctness gates for the integer and copy changes
-------------------------------------------------

  python3 tests/cpu-throughput/check-integer.py --qemu /path/to/candidate \
      --cpu m68060 --out /tmp/integer-check
  python3 tests/cpu-throughput/check-memory-order.py --qemu /path/to/candidate \
      --cpu m68060 --out /tmp/order-check
  python3 tests/cpu-throughput/check-flag-migration.py --qemu /path/to/candidate \
      --receiver /path/to/pre-change/qemu --out /tmp/migration-check

check-integer.py verifies 1,320 arithmetic, CCR, condition, preserved-X and
TB-boundary cases, including overflow/carry edges and CCR-read/BTST sequences.
The older baseline fails the latter sequence because inline flag flushing
did not invalidate a previously synchronized operation encoding.
check-memory-order.py checks actual optimized TCG output on x86-64: eight
MOVEM reads share one StoreLoad fence, and intervening stores, helper calls
and control-flow boundaries retain the required fences. It also executes
MMIO reads. It is an emitted-code regression, not a proof of every concurrent
execution or of weak-host memory ordering.
check-flag-migration.py pauses a guest in DBRA loops that preserve the lazy
arithmetic flags, migrates it to the older receiver, and verifies all four
ADDQ/SUBQ carry/overflow edge states there. The sender materializes new lazy
encodings before saving. It requires local QMP socket access. These gates
accept --cpu and --machine overrides; migration additionally needs a receiver
supporting that model. Each output directory must be fresh.
