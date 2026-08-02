; NSIS installer for the self-contained Windows package.
;
; Build with:
;   makensis -DBUNDLE=<path to assembled tree> -DOUTFILE=<setup.exe> shinogi.nsi
;
; The bundle tree is expected to contain shinogi.exe, emutos-virt.elf,
; the qemu\ subtree, and the diagnostic probe.

!ifndef BUNDLE
  !error "BUNDLE not defined"
!endif
!ifndef OUTFILE
  !define OUTFILE "shinogi-setup.exe"
!endif

Name "shinogi"
OutFile "${OUTFILE}"
Unicode True
InstallDir "$PROGRAMFILES64\shinogi"
InstallDirRegKey HKLM "Software\shinogi" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show

!include "MUI2.nsh"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\shinogi.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch shinogi now"

!insertmacro MUI_PAGE_LICENSE "${BUNDLE}\qemu\COPYING"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "shinogi" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  File "${BUNDLE}\shinogi.exe"
  File "${BUNDLE}\emutos-virt.elf"
  File "${BUNDLE}\sdl-grab-probe.exe"
  File "${BUNDLE}\SDL2.dll"
  File "README.txt"

  SetOutPath "$INSTDIR\qemu"
  File /r "${BUNDLE}\qemu\*.*"

  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\shinogi"
  CreateShortcut "$SMPROGRAMS\shinogi\shinogi.lnk" "$INSTDIR\shinogi.exe"
  CreateShortcut "$SMPROGRAMS\shinogi\shinogi (GTK display).lnk" \
                 "$INSTDIR\shinogi.exe" "gtk"
  CreateShortcut "$SMPROGRAMS\shinogi\Read me.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\shinogi\Uninstall.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut "$DESKTOP\shinogi.lnk" "$INSTDIR\shinogi.exe"

  WriteRegStr HKLM "Software\shinogi" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\shinogi" \
    "DisplayName" "shinogi - Atari GEM on QEMU"
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

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\shinogi.lnk"
  Delete "$SMPROGRAMS\shinogi\shinogi.lnk"
  Delete "$SMPROGRAMS\shinogi\shinogi (GTK display).lnk"
  Delete "$SMPROGRAMS\shinogi\Read me.lnk"
  Delete "$SMPROGRAMS\shinogi\Uninstall.lnk"
  RMDir "$SMPROGRAMS\shinogi"

  Delete "$INSTDIR\shinogi.exe"
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
