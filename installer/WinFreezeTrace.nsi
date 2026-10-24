
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"
!insertmacro GetParameters
!insertmacro GetOptions
!insertmacro GetSize

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef VERSION_NUM
  !define VERSION_NUM "0.0.0.0"
!endif
!ifndef SRCDIR
  !define SRCDIR ".."
!endif
!ifndef ARCH
  !define ARCH "x64"
!endif
!ifndef EXE
  !define EXE "${SRCDIR}\build\Release\WinFreezeTrace.exe"
!endif
!ifndef DLL
  !define DLL "${SRCDIR}\build\Release\WinFreezeTraceCore.dll"
!endif
; Folder with the published WinFreezeTrace Manager (dotnet publish output). Leave it empty to build an installer
; that has no Manager component (recorder only).
!ifndef MGRDIR
  !define MGRDIR ""
!endif
!if "${MGRDIR}" != ""
  !define HAVE_MANAGER
!endif
!define MGR_EXE "WinFreezeTraceManager.exe"
!ifndef OUTFILE
  !define OUTFILE "WinFreezeTrace-Setup-${VERSION}-${ARCH}.exe"
!endif

!define APP "WinFreezeTrace"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"

Name "${APP} ${VERSION} (${ARCH})"
OutFile "${OUTFILE}"
!if "${ARCH}" == "x64"
  InstallDir "$PROGRAMFILES64\${APP}"
!else
  InstallDir "$PROGRAMFILES\${APP}"
!endif
InstallDirRegKey HKLM "Software\${APP}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION_NUM}"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!define MUI_ABORTWARNING
; Separate icons: Setup.exe (installer) and Uninstall.exe (uninstaller).
!define MUI_ICON "${SRCDIR}\assets\setup.ico"
!define MUI_UNICON "${SRCDIR}\assets\uninstall.ico"
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_HEADER_TEXT "License"
!define MUI_PAGE_HEADER_SUBTEXT "${APP} is free software released under the BSD Zero Clause License."
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE"

; Terms of Use: must be accepted. Text comes from TERMS.txt (also installed next to the program).
!define MUI_PAGE_HEADER_TEXT "Terms of Use"
!define MUI_PAGE_HEADER_SUBTEXT "Please review the terms of use before installing ${APP}."
!define MUI_LICENSEPAGE_TEXT_TOP "Press Page Down to see the rest of the terms."
!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "I have read and agree to the Terms of Use"
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\TERMS.txt"

; Privacy Notice: information only, nothing to accept. Text comes from PRIVACY.txt.
!define MUI_PAGE_HEADER_TEXT "Privacy Notice"
!define MUI_PAGE_HEADER_SUBTEXT "What ${APP} records and where it is stored."
!define MUI_LICENSEPAGE_TEXT_TOP "Press Page Down to see the rest of the notice."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "${APP} does not send data over the network. Click Next to continue."
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\PRIVACY.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"


Function un.onInit
!if "${ARCH}" == "x64"
  SetRegView 64
!endif
FunctionEnd

Section "${APP} (required)" SecMain
  SectionIn RO
  ; Upgrade: ask the running Manager and recorder of this session to quit so their files can be replaced.
  IfFileExists "$INSTDIR\Manager\${MGR_EXE}" 0 +3
    nsExec::Exec '"$INSTDIR\Manager\${MGR_EXE}" --exit'
    Pop $0
  IfFileExists "$INSTDIR\WinFreezeTrace.exe" 0 +3
    nsExec::Exec '"$INSTDIR\WinFreezeTrace.exe" --exit'
    Pop $0
  ${If} ${FileExists} "$INSTDIR\WinFreezeTrace.exe"
    Sleep 1500   ; give the old recorder/Manager time to quit so their files can be replaced
  ${EndIf}
  SetOutPath "$INSTDIR"
  File "${EXE}"
  File "${DLL}"
  File "${SRCDIR}\assets\winfreezetrace.ico"
  File "${SRCDIR}\LICENSE"
  File "${SRCDIR}\TERMS.txt"
  File "${SRCDIR}\PRIVACY.txt"
  File "${SRCDIR}\README.md"
  File "${SRCDIR}\CHANGELOG.md"
  File "${SRCDIR}\install_task.cmd"
  File "${SRCDIR}\uninstall_task.cmd"
  File "${SRCDIR}\register_task.ps1"

  File "${SRCDIR}\installer\path.ps1"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "Software\${APP}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "TamKungZ_"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\winfreezetrace.ico"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/TamKungZ/WinFreezeTrace"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

!ifdef HAVE_MANAGER
; Selected by default. It is a separate program: the recorder works without it (no tray icon, no window).
Section "WinFreezeTrace Manager (tray icon and control window)" SecManager
  SetOutPath "$INSTDIR\Manager"
  File /r "${MGRDIR}\*.*"
  SetOutPath "$INSTDIR"
SectionEnd
!endif

Section "Add to system PATH (run `WinFreezeTrace` from any terminal)" SecPath
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\path.ps1" -Action add -Dir "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    DetailPrint "Could not update PATH (exit code $0)."
  ${Else}
    WriteRegDWORD HKLM "Software\${APP}" "PathAdded" 1
    System::Call 'user32::SendMessageTimeoutW(i 0xffff, i 0x1A, i 0, w "Environment", i 2, i 5000, *i .r0)'
  ${EndIf}
SectionEnd

Section /o "Start with Windows for all users (Startup apps)" SecStartup
!ifdef HAVE_MANAGER
  ${If} ${SectionIsSelected} ${SecManager}
    ; The Manager starts hidden in the tray and then starts the recorder.
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager" '"$INSTDIR\Manager\${MGR_EXE}" --background'
  ${Else}
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}" '"$INSTDIR\WinFreezeTrace.exe"'
  ${EndIf}
!else
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}" '"$INSTDIR\WinFreezeTrace.exe"'
!endif
  WriteRegDWORD HKLM "Software\${APP}" "StartupAdded" 1
SectionEnd

Section "Start Menu shortcuts" SecShortcuts
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\${APP}"
!ifdef HAVE_MANAGER
  ${If} ${SectionIsSelected} ${SecManager}
    CreateShortcut "$SMPROGRAMS\${APP}\${APP} Manager.lnk" "$INSTDIR\Manager\${MGR_EXE}" "" "$INSTDIR\winfreezetrace.ico"
  ${Else}
    CreateShortcut "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk" "$INSTDIR\WinFreezeTrace.exe" "" "$INSTDIR\winfreezetrace.ico"
  ${EndIf}
!else
  CreateShortcut "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk" "$INSTDIR\WinFreezeTrace.exe" "" "$INSTDIR\winfreezetrace.ico"
!endif
  CreateShortcut "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

; Hidden: runs last and records the installed size for Settings > Apps.
Section "-Post"
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

; .onInit uses ${SecManager}, so it must come after the Section definitions.
Function .onInit
!if "${ARCH}" == "x64"
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This is the 64-bit build of ${APP}. Use the x86 installer on 32-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
!endif
!ifdef HAVE_MANAGER
  ; Silent installs: /MANAGER=0 skips the Manager (it is installed by default).
  ${GetParameters} $R0
  ${GetOptions} $R0 "/MANAGER=" $R1
  ${If} $R1 == "0"
    !insertmacro UnselectSection ${SecManager}
  ${EndIf}
!endif
FunctionEnd

LangString DESC_Main ${LANG_ENGLISH} "The recorder (WinFreezeTrace.exe + WinFreezeTraceCore.dll), documentation and the optional logon-task scripts. It has no window or tray icon of its own."
LangString DESC_Manager ${LANG_ENGLISH} "WinFreezeTrace Manager: a tray icon and control window to turn monitoring on/off, capture a report, browse reports and change settings. Optional: the recorder works without it."
LangString DESC_Path ${LANG_ENGLISH} "Adds the install folder to the machine PATH so WinFreezeTrace can be called by name."
LangString DESC_Startup ${LANG_ENGLISH} "Starts WinFreezeTrace when any user signs in (through the Manager if it is installed, otherwise the recorder alone). It then appears in Settings > Apps > Startup, where it can be switched off. Each user can also toggle it in the Manager's settings."
LangString DESC_SM ${LANG_ENGLISH} "Start Menu entries for all users."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_Main)
!ifdef HAVE_MANAGER
  !insertmacro MUI_DESCRIPTION_TEXT ${SecManager} $(DESC_Manager)
!endif
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_Path)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartup} $(DESC_Startup)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} $(DESC_SM)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  SetShellVarContext all

  IfFileExists "$INSTDIR\Manager\${MGR_EXE}" 0 +3
    nsExec::Exec '"$INSTDIR\Manager\${MGR_EXE}" --exit'
    Pop $0
  IfFileExists "$INSTDIR\WinFreezeTrace.exe" 0 +3
    nsExec::Exec '"$INSTDIR\WinFreezeTrace.exe" --exit'
    Pop $0
  Sleep 1500   ; let the Manager and recorder finish quitting so their files can be deleted
  nsExec::Exec '"$SYSDIR\schtasks.exe" /End /TN "${APP}"'
  Pop $0
  nsExec::Exec '"$SYSDIR\schtasks.exe" /Delete /F /TN "${APP}"'
  Pop $0

  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}"            ; 0.5.x entry of the installing user
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager"   ; the installing user's Manager setting
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "${APP}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "${APP} Manager"

  ; The Manager's "Start with Windows" is a per-user Run entry. The uninstaller runs elevated and may be another account,
  ; so also clean every user profile that is loaded (signed in). Profiles of users who are signed out are not loaded and
  ; cannot be reached; their entry then points at a missing file and is harmless.
  StrCpy $2 0
  user_loop:
    EnumRegKey $3 HKU "" $2
    StrCmp $3 "" user_done
    IntOp $2 $2 + 1
    StrCpy $4 $3 8
    StrCmp $4 "S-1-5-21" 0 user_loop
    DeleteRegValue HKU "$3\Software\Microsoft\Windows\CurrentVersion\Run" "${APP}"
    DeleteRegValue HKU "$3\Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager"
    DeleteRegValue HKU "$3\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "${APP}"
    DeleteRegValue HKU "$3\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "${APP} Manager"
    Goto user_loop
  user_done:

  ReadRegDWORD $1 HKLM "Software\${APP}" "PathAdded"
  ${If} $1 == 1
    nsExec::Exec '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\path.ps1" -Action remove -Dir "$INSTDIR"'
    Pop $0
    System::Call 'user32::SendMessageTimeoutW(i 0xffff, i 0x1A, i 0, w "Environment", i 2, i 5000, *i .r0)'
  ${EndIf}

  Delete /REBOOTOK "$INSTDIR\WinFreezeTrace.exe"
  Delete /REBOOTOK "$INSTDIR\WinFreezeTraceCore.dll"
  RMDir /r /REBOOTOK "$INSTDIR\Manager"
  Delete "$INSTDIR\winfreezetrace.ico"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\TERMS.txt"
  Delete "$INSTDIR\PRIVACY.txt"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\CHANGELOG.md"
  Delete "$INSTDIR\install_task.cmd"
  Delete "$INSTDIR\uninstall_task.cmd"
  Delete "$INSTDIR\register_task.ps1"
  Delete "$INSTDIR\path.ps1"
  Delete "$INSTDIR\Uninstall.exe"
  SetOutPath "$TEMP"   ; the working directory cannot be removed, and it starts out as the install folder
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APP}\${APP}.lnk"
  Delete "$SMPROGRAMS\${APP}\${APP} Manager.lnk"
  Delete "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk"
  Delete "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk"
  RMDir "$SMPROGRAMS\${APP}"

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\${APP}"
SectionEnd
