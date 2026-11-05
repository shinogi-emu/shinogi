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
!ifndef EDITION
  !error "EDITION not defined - build through tools/make-windows-package.sh"
!endif

Name "${EDITION} ${VERSION}"
OutFile "${OUTFILE}"
Unicode True
InstallDir "$PROGRAMFILES64\${EDITION}"
InstallDirRegKey HKLM "Software\shinogi\Editions\${EDITION}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

Var DriveC
Var DriveCField
Var SettingsFile

Function .onInit
  SetRegView 64
  ReadRegStr $0 HKLM "Software\shinogi\Editions\${EDITION}" "InstallDir"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
  StrCpy $SettingsFile "$APPDATA\${EDITION}\settings.ini"
  ReadINIStr $DriveC $SettingsFile "Drives" "C"
  ${If} $DriveC == ""
    StrCpy $DriveC "$PROFILE\${EDITION}\drive-c"
  ${EndIf}
  ; Also usable for unattended installation: /DRIVEC="D:\Atari\Drive C"
  ${GetParameters} $0
  ${GetOptions} $0 "/DRIVEC=" $1
  ${If} $1 != ""
    StrCpy $DriveC $1
  ${EndIf}
FunctionEnd

Function DriveCPage
  !insertmacro MUI_HEADER_TEXT "Atari drive C" "Choose the Windows folder used as drive C."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 30u "Choose your existing drive C folder, or an empty folder for a new installation. Your personal files and configuration in an existing drive C are kept."
  Pop $0
  ${NSD_CreateDirRequest} 0 40u 78% 14u "$DriveC"
  Pop $DriveCField
  SendMessage $DriveCField ${EM_SETLIMITTEXT} 220 0
  ${NSD_CreateBrowseButton} 80% 39u 20% 16u "Browse..."
  Pop $0
  ${NSD_OnClick} $0 BrowseDriveC
  ${NSD_CreateLabel} 0 70u 100% 40u "You can change this folder, startup resolution and full screen later using Shinogi Settings in the Start menu. Changing the folder does not move your files."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function BrowseDriveC
  Pop $0
  ${NSD_GetText} $DriveCField $0
  nsDialogs::SelectFolderDialog "Choose Atari drive C" "$0"
  Pop $0
  ${If} $0 != error
    ${NSD_SetText} $DriveCField $0
  ${EndIf}
FunctionEnd

Function DriveCLeave
  ${NSD_GetText} $DriveCField $DriveC
  Call ValidateDriveC
FunctionEnd

Function ValidateDriveC
  StrLen $0 $DriveC
  ${If} $0 < 3
  ${OrIf} $0 > 220
    MessageBox MB_ICONSTOP "Choose a full drive C folder path of at most 220 characters."
    Abort
  ${EndIf}
  ; Require an absolute drive path or UNC share, before normalizing it.
  StrCpy $0 $DriveC 2
  StrCpy $1 $DriveC 2 1
  ${If} $0 != "\\"
  ${AndIf} $1 != ":\"
    MessageBox MB_ICONSTOP "Choose a full folder path, such as D:\Atari\Drive C."
    Abort
  ${EndIf}
  ; NSIS GetFullPathName uses long-name expansion and can fail for a
  ; folder that does not exist yet. Win32 normalization supports new paths.
  System::Call 'kernel32::GetFullPathNameW(w "$DriveC", i ${NSIS_MAX_STRLEN}, w .r2, p 0) i .r0'
  ${If} $0 == 0
  ${OrIf} $0 >= ${NSIS_MAX_STRLEN}
    MessageBox MB_ICONSTOP "Could not resolve the drive C folder path."
    Abort
  ${EndIf}
  StrCpy $DriveC $2
  StrLen $0 $DriveC
  ${If} $0 > 220
    MessageBox MB_ICONSTOP "The full folder path is too long. Choose a shorter path."
    Abort
  ${EndIf}
  System::Call 'kernel32::GetFileAttributesW(w "$DriveC") i .r0'
  ${If} $0 != -1
    IntOp $0 $0 & 0x10
    ${If} $0 == 0
      MessageBox MB_ICONSTOP "The drive C path points to a file. Choose a folder."
      Abort
    ${EndIf}
  ${EndIf}
  System::Call 'kernel32::GetFullPathNameW(w "$INSTDIR", i ${NSIS_MAX_STRLEN}, w .r2, p 0) i .r0'
  ${If} $0 == 0
  ${OrIf} $0 >= ${NSIS_MAX_STRLEN}
    MessageBox MB_ICONSTOP "Could not resolve the program folder path."
    Abort
  ${EndIf}
  StrLen $0 $2
  StrCpy $1 $DriveC $0
  ${If} $1 == $2
    StrCpy $1 $DriveC 1 $0
    ${If} $1 == ""
    ${OrIf} $1 == "\"
      MessageBox MB_ICONSTOP "Choose a drive C folder outside the Shinogi program folder."
      Abort
    ${EndIf}
  ${EndIf}
  ; Do not seed a nonempty unrelated folder with the bundled system.
  IfFileExists "$DriveC\AUTO\MINT.PRG" validDriveC
  IfFileExists "$DriveC\shinogi-drive-c\AUTO\MINT.PRG" nestedDriveC
  FindFirst $0 $1 "$DriveC\*.*"
  ${DoWhile} $1 != ""
    ${If} $1 != "."
    ${AndIf} $1 != ".."
      FindClose $0
      MessageBox MB_ICONSTOP "This folder is not a drive C installation and is not empty. Choose the folder containing AUTO and MINT, or an empty folder."
      Abort
    ${EndIf}
    FindNext $0 $1
  ${Loop}
  FindClose $0
  Goto validDriveC
  nestedDriveC:
    MessageBox MB_ICONSTOP "Your drive C is one folder further down. Choose $DriveC\shinogi-drive-c instead."
    Abort
  validDriveC:
FunctionEnd

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
Page custom DriveCPage DriveCLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "shinogi" SecMain
  SectionIn RO
  ; A custom installation folder must never replace another CPU edition.
  ReadINIStr $0 "$INSTDIR\edition.ini" "Install" "Edition"
  ${If} $0 != ""
  ${AndIf} $0 != "${EDITION}"
    MessageBox MB_ICONSTOP "This folder belongs to $0. Choose a separate ${EDITION} program folder."
    Abort
  ${EndIf}
  ; Custom pages do not run for /S installations.
  Call ValidateDriveC
  ; Check the selected destination before replacing the installed app.
  ClearErrors
  CreateDirectory "$DriveC"
  GetTempFileName $0 "$DriveC"
  IfErrors driveCNotWritable
  Delete $0
  Goto driveCWritable
  driveCNotWritable:
    MessageBox MB_ICONSTOP "Cannot write to $DriveC. Choose a writable drive C folder."
    Abort
  driveCWritable:
  ; Stop this edition only; other CPU editions may be running alongside it.
  IfFileExists "$INSTDIR\shinogi.exe" 0 editionStopped
    nsExec::Exec '"$INSTDIR\shinogi.exe" --stop-edition'
    Pop $0
    ${If} $0 != 0
      MessageBox MB_ICONSTOP "Close ${EDITION} before updating this installation."
      Abort
    ${EndIf}
  editionStopped:

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
  SetOutPath "$INSTDIR\network"
  File /r "${BUNDLE}\network\*.*"
  SetOutPath "$INSTDIR"

  ; A first install lays down the whole system tree -- EmuTOS, fVDI,
  ; FreeMiNT, XaAES and the bundled GEM applications -- so one download
  ; produces a bootable drive C with no second zip to unpack by hand.
  ;
  ; ONLY when there is no drive C yet. An existing one is shared space:
  ; our files, the user's files and their edited configs sit in the same
  ; folder, so re-extracting over it would silently take their FVDI.SYS
  ; and anything else they had changed. That case gets the three overlays
  ; below and nothing more, which is what every build up to beta8 did.
  ; Reconciling a tree the user has edited is shin-apn.20.
  ClearErrors
  IfFileExists "$DriveC\AUTO\MINT.PRG" driveCExists 0
    DetailPrint "Creating drive C at $DriveC"
    SetOutPath "$DriveC"
    File /r "${BUNDLE}\drive-c\*.*"
    SetOutPath "$INSTDIR"
  driveCExists:

  ; DHCP makes optional LAN mode automatic inside Atari as well. These
  ; files do not run in Internet mode and do not change Windows networking.
  SetOutPath "$DriveC\MINT\1-19-CUR\SYS-ROOT\BIN"
  File /oname=DHCLIENT "${BUNDLE}\DHCLIENT"
  SetOutPath "$DriveC\MINT\1-19-CUR"
  File /oname=SHNDHCP.SH "${BUNDLE}\SHNDHCP.SH"
  SetOutPath "$INSTDIR"

  ; Both CPU editions carry the updated virtio-net driver. Put it where
  ; the existing FreeMiNT tree loads it so neither edition is left using
  ; the older one-second receive poll.
  !ifdef INCLUDE_NET_DRIVER
    SetOutPath "$DriveC\MINT\1-19-CUR"
    File /oname=VIRTIONE.XIF "${BUNDLE}\VIRTIONE.XIF"
    SetOutPath "$INSTDIR"
  !endif

  ; Keep the FreeMiNT kernel paired with the CPU selected by this edition.
  ; The 060 compiler can emit instructions removed from the 040, while a 040
  ; build needlessly exercises the 060 compatibility package.
  SetOutPath "$DriveC\AUTO"
  File /oname=MINT.PRG "${BUNDLE}\MINT.PRG"
  SetOutPath "$INSTDIR"

  ; A stock 68060 traps instructions that were implemented by earlier
  ; processors. Install the standard software package before fVDI and
  ; FreeMiNT so existing 68040 applications retain their semantics.
  !ifdef INCLUDE_060SP
    SetOutPath "$DriveC\AUTO"
    File /oname=060SP.PRG "${BUNDLE}\060SP.PRG"
    SetOutPath "$INSTDIR"
  !else
    ; This is a package-managed file. Remove it when switching an existing
    ; drive C back from the 060 edition to the 040 edition.
    Delete "$DriveC\AUTO\060SP.PRG"
  !endif

  IfErrors 0 driveCInstalled
    MessageBox MB_ICONSTOP "Could not install the system files in $DriveC. Check folder permissions and available space, then run the installer again."
    Abort
  driveCInstalled:

  SetOutPath "$INSTDIR\qemu"
  File /r "${BUNDLE}\qemu\*.*"

  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\${EDITION}"
  CreateShortcut "$SMPROGRAMS\${EDITION}\${EDITION}.lnk" "$INSTDIR\shinogi.exe"
  CreateShortcut "$SMPROGRAMS\${EDITION}\${EDITION} Settings.lnk" "$INSTDIR\shinogi.exe" "--settings"
  CreateShortcut "$SMPROGRAMS\${EDITION}\Read me.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\${EDITION}\Uninstall.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut "$DESKTOP\${EDITION}.lnk" "$INSTDIR\shinogi.exe"
  CreateShortcut "$SMPROGRAMS\${EDITION}\${EDITION} drive C.lnk" "$INSTDIR\shinogi.exe" "--open-drive-c"
  CreateShortcut "$DESKTOP\${EDITION} drive C.lnk" "$INSTDIR\shinogi.exe" "--open-drive-c"

  CreateDirectory "$APPDATA\${EDITION}"
  ClearErrors
  WriteINIStr $SettingsFile "Drives" "C" "$DriveC"
  IfErrors 0 settingsSaved
    MessageBox MB_ICONSTOP "Could not save the drive C location in $SettingsFile."
    Abort
  settingsSaved:

  WriteRegStr HKLM "Software\shinogi\Editions\${EDITION}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\shinogi\Editions\${EDITION}" "Version" "${VERSION}"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}" \
    "DisplayName" "${EDITION} ${VERSION} - Atari GEM on QEMU"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}" \
    "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}" \
    "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}" \
    "NoModify" 1
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}" \
    "NoRepair" 1
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}" \
    "DisplayVersion" "${VERSION}"

  WriteINIStr "$INSTDIR\edition.ini" "Install" "Edition" "${EDITION}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  ; Restore only the bridge registered by Shinogi before removing its
  ; setup helper. A shared TAP driver is never uninstalled here.
  SetRegView 64
  StrCpy $2 0
  StrCpy $3 0
  editionLoop:
    EnumRegKey $4 HKLM "Software\shinogi\Editions" $3
    ${If} $4 == ""
      Goto editionLoopEnd
    ${EndIf}
    ${If} $4 != "${EDITION}"
      StrCpy $2 1
    ${EndIf}
    IntOp $3 $3 + 1
    Goto editionLoop
  editionLoopEnd:
  ReadRegStr $5 HKLM "Software\shinogi" "InstallDir"
  ${If} $5 != ""
    StrCpy $2 1 ; a legacy shared installation may still use the bridge
  ${EndIf}
  ReadRegStr $0 HKLM "Software\shinogi\LAN" "EthernetGuid"
  ${If} $0 != ""
  ${AndIf} $2 == 0
    ${DisableX64FSRedirection}
    nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\network\lan-setup.ps1" -EthernetGuid $0 -Remove'
    Pop $1
    ${EnableX64FSRedirection}
    ${If} $1 != "0"
      MessageBox MB_ICONSTOP "Could not restore Ethernet before uninstalling. Open Shinogi Settings and choose Remove LAN, then uninstall again. See %PROGRAMDATA%\shinogi\lan-setup.log."
      Abort
    ${EndIf}
  ${EndIf}
  nsExec::Exec '"$INSTDIR\shinogi.exe" --stop-edition'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONSTOP "Close ${EDITION} before uninstalling."
    Abort
  ${EndIf}
  Delete "$DESKTOP\${EDITION}.lnk"
  Delete "$DESKTOP\${EDITION} drive C.lnk"
  Delete "$SMPROGRAMS\${EDITION}\${EDITION}.lnk"
  Delete "$SMPROGRAMS\${EDITION}\${EDITION} drive C.lnk"
  Delete "$SMPROGRAMS\${EDITION}\${EDITION} Settings.lnk"
  Delete "$SMPROGRAMS\${EDITION}\Read me.lnk"
  Delete "$SMPROGRAMS\${EDITION}\Uninstall.lnk"
  RMDir "$SMPROGRAMS\${EDITION}"

  Delete "$INSTDIR\shinogi.exe"
  Delete "$INSTDIR\shinogi-hostfsd.exe"
  Delete "$INSTDIR\emutos-virt.elf"
  Delete "$INSTDIR\sdl-grab-probe.exe"
  Delete "$INSTDIR\SDL2.dll"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\BUILD.txt"
  Delete "$INSTDIR\edition.ini"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR\qemu"
  ; Application upgrades leave the host bridge intact; uninstall restored
  ; its Ethernet configuration above. The shared TAP driver is kept.
  RMDir /r "$INSTDIR\network"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${EDITION}"
  DeleteRegKey HKLM "Software\shinogi\Editions\${EDITION}"
SectionEnd
