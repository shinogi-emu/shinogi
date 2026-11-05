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

Additional drive checks
------------------------

test_drives.c links the real drives.c and the launcher's resource object.
It checks slot switching, OK/Cancel, read-only defaults, INI persistence and
removal, file identity validation and escaped image paths. Cross-compile
with -lcomdlg32 -lshell32 -lole32 and run on a disposable Windows profile.

test_editions.c accepts three edition/installer pairs, the compiled guest
windows-boot.c probe, a formatted raw hard image and a720KB/1.44MB raw floppy.
The image SAME.TXT contains WINDOWS IMAGE followed by LF; the floppy's
SAME.TXT contains WINDOWS FLOPPY followed by LF. It installs all editions,
checks separate default folders/settings/desktop and Start shortcuts, boots
all three actual Windows launchers/helpers/QEMU simultaneously headlessly,
and requires guest folder/image/floppy reads and host-verified folder writes.
Upgrading/uninstalling the first edition must leave the other running
editions, shortcuts, settings and guest files intact. Link with -lshell32
-lole32 -luuid. This requires an isolated profile, never a user's desktop.

SDL keyboard grab regression
----------------------------

test_sdl_keyboard.c links SDL2 and window-controls.c. Run beside the complete
DLL set from the assembled package, whose official SDL2.dll replaces the
build tree's SDL2 compatibility DLL.
--baseline must fail because QEMU's SDL_GRAB_KEYBOARD=1 captures modifiers;
normal mode must pass with keyboard grab off and mouse grab still on. This
checks actual SDL grab state even when Wine's hotkey behavior differs from
Windows. The launcher applies this environment override before CreateProcess.
