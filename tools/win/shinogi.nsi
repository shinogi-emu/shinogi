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

Name "shinogi ${VERSION}"
OutFile "${OUTFILE}"
Unicode True
InstallDir "$PROGRAMFILES64\shinogi"
InstallDirRegKey HKLM "Software\shinogi" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show

!include "MUI2.nsh"

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
  ; Clear a helper left over from a previous session before writing any
  ; files. Up to b8 the helper kept listening after the guest went away,
  ; so a launcher that was killed rather than closed left it running and
  ; holding shinogi-hostfsd.exe open -- the installer then stopped with
  ; "Error opening file for writing". The launcher now passes --once so
  ; it exits with the guest, but an older orphan can still be running on
  ; a machine being upgraded, and it is our own process to end.
  nsExec::Exec 'taskkill /F /IM shinogi-hostfsd.exe'
  Pop $0

  SetOutPath "$INSTDIR"

  File "${BUNDLE}\shinogi.exe"
  File "${BUNDLE}\shinogi-hostfsd.exe"
  File "${BUNDLE}\emutos-virt.elf"
  File "${BUNDLE}\sdl-grab-probe.exe"
  File "${BUNDLE}\SDL2.dll"
  File "README.txt"

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
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR\qemu"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi"
  DeleteRegKey HKLM "Software\shinogi"
SectionEnd
