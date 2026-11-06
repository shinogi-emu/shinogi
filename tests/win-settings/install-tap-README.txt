Targeted TAP installer checks
============================

Run:

    python3 tests/win-settings/check_install_tap.py

The native fixture includes the real tools/win/install-tap.c transaction and
JSON writer. Only the Windows API boundary and platform wrappers are replaced.
The fixture constrains every mutation to the one fresh device element, requires
DI_ENUMSINGLEINF pointing at the staged INF, and supplies no global driver-update
API. It checks all transaction-stage failures, partial registration failure,
failed removal/class-parameter setup, clean removal, restart-pending removal,
valid/missing/malformed NetCfgInstanceId, restart flags, and exact recovery
identity. It accounts for all device information sets, driver lists and registry
keys. A separate pipe handshake proves the initial identity is flushed before
the first mutating API; a broken-stdout test proves failure to publish that
identity stops before mutation.

The driver transaction runs under AddressSanitizer and UndefinedBehaviorSanitizer.
LeakSanitizer is disabled because this sandbox uses ptrace; explicit API resource
accounting remains active. Builds/logs are kept in the printed /tmp directory.
The runner also links x64 and x86 executables against actual MinGW Windows SDK
headers/libraries with -Wall -Wextra -Werror. Only the x64 executable is intended
for the pinned amd64 driver package. No actual driver is installed by these tests.

Protocol
--------

    install-tap.exe <path-to-OemVista.inf>

Caller elevates explicitly. The helper rejects a nonelevated token and accepts
only the exact official tap-windows6 9.27.0 win10/amd64 INF hash:

    1327ab3a8c50691f04bea8e2ca356c5b604092a719e219464f8cc4b42e192de9

It checks the source INF and rechecks the published INF after SetupCopyOEMInf.
Windows validates the package catalog and kernel driver during staging/install.
The PS caller separately gates existing shared tap0901 service/binary versions;
per-device targeting cannot isolate two different builds of one shared service.

Stdout is JSON Lines, diagnostics are stderr. Before the first mutation, the
helper emits and flushes progress:true with a UUID-based ROOT\AtariTAP\{UUID}
identity. It then emits a final progress:false record. Callers parse the LAST
record after process completion. If the process times out, the initial record
provides exact identity to inspect/recover; it is not a success indication.

Exit 0: installed with GUID, no restart needed.
Exit 1: installed, restart required; GUID may be null until after restart.
Exit 2: failure. A fully cleaned rollback has deviceInstanceId:null. A failed or
restart-pending own-node removal retains deviceInstanceId and recoveryRequired:
true; rebootRequired identifies a known restart requirement. No driver package
or unrelated adapter is removed. Never interpret progress alone as success.

The test replaces cryptographic hashing, UUID generation, DLL loading and token
membership wrappers; those compile against real SDK APIs but need live Windows
verification. The actual Cygwin/Windows setup, signed-driver trust, callback
behavior, shared-service compatibility guard and Wi-Fi forwarding are not proven
by native mock tests.

API references used for the sequence:
https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupcopyoeminfa
https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdicreatedeviceinfow
https://learn.microsoft.com/en-us/windows/win32/api/setupapi/ns-setupapi-sp_devinstall_params_a
https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdibuilddriverinfolist
https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diinstalldevice
https://learn.microsoft.com/en-us/windows-hardware/drivers/install/dif-remove
