; NSIS installer for the self-contained Windows package.
;
; Build with:
;   makensis -DBUNDLE=<path to assembled tree> -DOUTFILE=<setup.exe> shinogi.nsi
;
; The bundle tree is expected to contain shinogi.exe,
; shinogi-hostfsd.exe, emutos-virt.elf, the qemu\ subtree, and the
; diagnostic probe.
;
; shinogi-hostfsd.exe is not optional: it is the host end of drive C and
; the launcher starts it. Installed beside shinogi.exe, which is where
; the launcher looks for it.

!ifndef BUNDLE
  !error "BUNDLE not defined"
!endif
!ifndef OUTFILE
  !define OUTFILE "shinogi-setup.exe"
!endif
!ifndef VERSION
  !error "VERSION not defined - build through tools/make-windows-package.sh"
!endif
!ifndef CPU
  !error "CPU not defined - build through tools/make-windows-package.sh"
!endif

Name "shinogi ${VERSION} (${CPU})"
OutFile "${OUTFILE}"
Unicode True
InstallDir "$PROGRAMFILES64\shinogi"
InstallDirRegKey HKLM "Software\shinogi" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show

!include "MUI2.nsh"

; The mountain, from assets/shinogi_logo.svg via tools/make-icons.py.
; ICON comes from make-windows-package.sh, like BUNDLE and VERSION, rather
; than being reached for relative to NSISDIR -- that path is wherever the
; build host keeps NSIS and is nothing to do with this repository.
!ifndef ICON
  !error "ICON not defined - build through tools/make-windows-package.sh"
!endif
!define MUI_ICON   "${ICON}"
!define MUI_UNICON "${ICON}"

!define MUI_ABORTWARNING
; NO MUI_FINISHPAGE_RUN.
;
; Launching the emulator from the finish page failed on two separate
; releases, and the elevation workaround below it (handing the path to
; Explorer so the app did not inherit the installer's admin token) did
; not cure it. It is not reproducible off the user's machine, and all it
; ever saved was one double-click on an icon that the installer has just
; put on the desktop.
;
; An installer that reliably does nothing beats one that intermittently
; hangs, because the hang is the product's first impression. Anything
; started from an elevated installer inherits that whole class of
; problem, so the drive C folder is offered as a SHORTCUT below rather
; than as another thing to launch from here.

!insertmacro MUI_PAGE_LICENSE "${BUNDLE}\qemu\COPYING"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "shinogi" SecMain
  SectionIn RO
  ; Stop an installed copy before replacing it. Ending the launcher alone
  ; does not end its QEMU child, and either process can keep files open.
  nsExec::Exec 'taskkill /F /IM shinogi.exe'
  Pop $0
  nsExec::Exec 'taskkill /F /IM qemu-system-m68kw.exe'
  Pop $0
  nsExec::Exec 'taskkill /F /IM qemu-system-m68k.exe'
  Pop $0
  nsExec::Exec 'taskkill /F /IM shinogi-hostfsd.exe'
  Pop $0

  ; Replace the application as one coherent tree so DLLs and QEMU data from
  ; an older build cannot survive an upgrade. Require two Shinogi-specific
  ; files before deleting recursively: $INSTDIR is user-selectable, and a
  ; mistaken broad directory must never be treated as our private tree.
  IfFileExists "$INSTDIR\shinogi.exe" 0 install_files
  IfFileExists "$INSTDIR\qemu\qemu-system-m68kw.exe" 0 install_files
  RMDir /r "$INSTDIR"

install_files:
  SetOutPath "$INSTDIR"

  File "${BUNDLE}\shinogi.exe"
  File "${BUNDLE}\shinogi-hostfsd.exe"
  File "${BUNDLE}\emutos-virt.elf"
  File "${BUNDLE}\sdl-grab-probe.exe"
  File "${BUNDLE}\SDL2.dll"
  File "${BUNDLE}\README.txt"
  File "${BUNDLE}\BUILD.txt"

  ; Both CPU editions carry the updated virtio-net driver. Put it where
  ; the existing FreeMiNT tree loads it so neither edition is left using
  ; the older one-second receive poll.
  !ifdef INCLUDE_NET_DRIVER
    SetOutPath "$PROFILE\shinogi-drive-c\MINT\1-19-CUR"
    File /oname=VIRTIONE.XIF "${BUNDLE}\VIRTIONE.XIF"
    SetOutPath "$INSTDIR"
  !endif

  ; Keep the FreeMiNT kernel paired with the CPU selected by this edition.
  ; The 060 compiler can emit instructions removed from the 040, while a 040
  ; build needlessly exercises the 060 compatibility package.
  SetOutPath "$PROFILE\shinogi-drive-c\AUTO"
  File /oname=MINT.PRG "${BUNDLE}\MINT.PRG"
  SetOutPath "$INSTDIR"

  ; A stock 68060 traps instructions that were implemented by earlier
  ; processors. Install the standard software package before fVDI and
  ; FreeMiNT so existing 68040 applications retain their semantics.
  !ifdef INCLUDE_060SP
    SetOutPath "$PROFILE\shinogi-drive-c\AUTO"
    File /oname=060SP.PRG "${BUNDLE}\060SP.PRG"
    SetOutPath "$INSTDIR"
  !else
    ; This is a package-managed file. Remove it when switching an existing
    ; drive C back from the 060 edition to the 040 edition.
    Delete "$PROFILE\shinogi-drive-c\AUTO\060SP.PRG"
  !endif

  SetOutPath "$INSTDIR\qemu"
  File /r "${BUNDLE}\qemu\*.*"

  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\shinogi"
  CreateShortcut "$SMPROGRAMS\shinogi\shinogi.lnk" "$INSTDIR\shinogi.exe"
  CreateShortcut "$SMPROGRAMS\shinogi\Read me.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\shinogi\Uninstall.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut "$DESKTOP\shinogi.lnk" "$INSTDIR\shinogi.exe"

  CreateShortcut "$SMPROGRAMS\shinogi\shinogi drive C.lnk" \
                 "$WINDIR\explorer.exe" "%USERPROFILE%\shinogi-drive-c"
  CreateShortcut "$DESKTOP\shinogi drive C.lnk" \
                 "$WINDIR\explorer.exe" "%USERPROFILE%\shinogi-drive-c"

  WriteRegStr HKLM "Software\shinogi" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\shinogi" "Version" "${VERSION}"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "DisplayName" "shinogi ${VERSION} - Atari GEM on QEMU"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "NoModify" 1
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "NoRepair" 1
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "DisplayVersion" "${VERSION}"

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\shinogi.lnk"
  Delete "$DESKTOP\shinogi drive C.lnk"
  Delete "$SMPROGRAMS\shinogi\shinogi.lnk"
  Delete "$SMPROGRAMS\shinogi\shinogi drive C.lnk"
  ; Left behind by installs up to b7, which offered a GTK display entry.
  Delete "$SMPROGRAMS\shinogi\shinogi (GTK display).lnk"
  Delete "$SMPROGRAMS\shinogi\Read me.lnk"
  Delete "$SMPROGRAMS\shinogi\Uninstall.lnk"
  RMDir "$SMPROGRAMS\shinogi"

  Delete "$INSTDIR\shinogi.exe"
  Delete "$INSTDIR\shinogi-hostfsd.exe"
  Delete "$INSTDIR\emutos-virt.elf"
  Delete "$INSTDIR\sdl-grab-probe.exe"
  Delete "$INSTDIR\SDL2.dll"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\BUILD.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR\qemu"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi"
  DeleteRegKey HKLM "Software\shinogi"
SectionEnd
