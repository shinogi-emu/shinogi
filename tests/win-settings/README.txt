Windows settings checks
=======================

make test runs native resolution, path, networking and boot-hook tests.

test_window_controls.c exercises the real Windows shortcut module with
separate foreground-window processes. Cross-compile it with
tools/win/window-controls.c using x86_64-w64-mingw32-gcc, -O2 -Wall -Wextra
-Werror. Run the executable from a fresh writable directory on Windows or
Wine with an isolated desktop; it writes fixture counters beside itself.
Success verifies opening from the emulator process, refocusing an existing
Settings instance, reopening after close, clean exit, and untouched Ctrl+Alt+S
keystrokes in an unrelated foreground process. It explicitly grants synthetic
focus changes from the foreground fixture to respect Windows focus rules.

The module receives foreground notifications and messages while waiting for
QEMU; it uses no polling timer or injected keyboard hook. It registers the
shortcut only while the emulator process owns the foreground window.

test_package_window.c launches the actual package and opens its real Settings
dialog from SDL. Arguments are launcher.exe, a disposable drive folder, and
the edition/version substring in the window title. It refuses an existing
settings.ini: use a fresh test Windows profile. Optional --no-shortcut runs
the same startup/close path without opening Settings for a baseline control.

The isolated Wine/Xvfb run verifies opening and cancelling real Settings with
QEMU still active. Its final WM_CLOSE test does not terminate SDL, including
with the unchanged beta12 launcher and without invoking Settings. That
shutdown/progress limitation is separate from shortcut validation; the test
keeps reporting failure rather than treating an active process as clean exit.
Actual Windows SDL quit behavior needs the laptop smoke test.
