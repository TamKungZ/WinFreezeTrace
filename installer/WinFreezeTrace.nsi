
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh"
!insertmacro GetParameters
!insertmacro GetOptions
!insertmacro GetSize
!insertmacro VersionCompare

Var Params   ; command line, read once in .onInit

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
!define PROFILELIST "SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList"

; ProfileList and HKU are not redirected for 64-bit tools, so a 32-bit installer on 64-bit Windows must look at the 64-bit view.
!macro RegView64
!if "${ARCH}" == "x86"
  ${If} ${RunningX64}
    SetRegView 64
  ${EndIf}
!endif
!macroend
!macro RegViewRestore
!if "${ARCH}" == "x86"
  SetRegView 32
!endif
!macroend
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"

Name "${APP} ${VERSION} (${ARCH})"
OutFile "${OUTFILE}"
!if "${ARCH}" == "x64"
  InstallDir "$PROGRAMFILES64\${APP}"
!else
  InstallDir "$PROGRAMFILES\${APP}"
!endif
; The previous install folder is read in .onInit, after the registry view is chosen: InstallDirRegKey is evaluated before that,
; always in the 32-bit view, so a 64-bit install would never be found (and an x86 install would be picked up by mistake).
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
  ; Upgrade: make sure the old Manager and recorder are gone so their files can be replaced.
  Call StopApp
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
  WriteRegStr HKLM "Software\${APP}" "VersionNum" "${VERSION_NUM}"
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

  ; install_task.cmd keeps its own copy of the recorder in %LOCALAPPDATA%\WinFreezeTrace\bin and points the logon task at it.
  ; Update those copies (only where they already exist) so the task runs the new version too.
  !insertmacro RegView64
  StrCpy $1 0
  prof_loop:
    EnumRegKey $2 HKLM "${PROFILELIST}" $1
    StrCmp $2 "" prof_done
    IntOp $1 $1 + 1
    ReadRegStr $3 HKLM "${PROFILELIST}\$2" "ProfileImagePath"
    ExpandEnvStrings $3 $3
    StrCmp $3 "" prof_loop
    IfFileExists "$3\AppData\Local\WinFreezeTrace\bin\WinFreezeTrace.exe" 0 prof_loop
      ClearErrors
      CopyFiles /SILENT "$INSTDIR\WinFreezeTrace.exe" "$3\AppData\Local\WinFreezeTrace\bin"
      CopyFiles /SILENT "$INSTDIR\WinFreezeTraceCore.dll" "$3\AppData\Local\WinFreezeTrace\bin"
      IfErrors 0 +2
        DetailPrint "Could not update the copy in $3\AppData\Local\WinFreezeTrace\bin (is it running?)."
      Goto prof_loop
  prof_done:
  !insertmacro RegViewRestore
SectionEnd

!ifdef HAVE_MANAGER
; Selected by default. It is a separate program: the recorder works without it (no tray icon, no window).
Section "WinFreezeTrace Manager (tray icon and control window)" SecManager
  RMDir /r "$INSTDIR\Manager"   ; upgrade: drop files of the old version that no longer exist (the processes were stopped above)
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
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}"
  ${Else}
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}" '"$INSTDIR\WinFreezeTrace.exe"'
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager"
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
    Delete "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk"
  ${Else}
    CreateShortcut "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk" "$INSTDIR\WinFreezeTrace.exe" "" "$INSTDIR\winfreezetrace.ico"
    Delete "$SMPROGRAMS\${APP}\${APP} Manager.lnk"
  ${EndIf}
!else
  CreateShortcut "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk" "$INSTDIR\WinFreezeTrace.exe" "" "$INSTDIR\winfreezetrace.ico"
!endif
  CreateShortcut "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

; Hidden: runs last. A component that is NOT ticked is switched off if an earlier install had it, so a re-install always
; matches the checkboxes (the choices of the previous install are pre-ticked in .onInit). Also records the installed size.
Section "-Post"
  ${IfNot} ${SectionIsSelected} ${SecStartup}
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP}"
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager"
    DeleteRegValue HKLM "Software\${APP}" "StartupAdded"
  ${EndIf}

  ${IfNot} ${SectionIsSelected} ${SecPath}
    ReadRegDWORD $1 HKLM "Software\${APP}" "PathAdded"
    ${If} $1 == 1
      nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\path.ps1" -Action remove -Dir "$INSTDIR"'
      Pop $0
      ${If} $0 == 0
        DeleteRegValue HKLM "Software\${APP}" "PathAdded"
        System::Call 'user32::SendMessageTimeoutW(i 0xffff, i 0x1A, i 0, w "Environment", i 2, i 5000, *i .r0)'
      ${Else}
        DetailPrint "Could not remove the PATH entry (exit code $0)."
      ${EndIf}
    ${EndIf}
  ${EndIf}

!ifdef HAVE_MANAGER
  ${IfNot} ${SectionIsSelected} ${SecManager}
    ${If} ${FileExists} "$INSTDIR\Manager\${MGR_EXE}"
      RMDir /r "$INSTDIR\Manager"
    ${EndIf}
    SetShellVarContext all
    Delete "$SMPROGRAMS\${APP}\${APP} Manager.lnk"
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "${APP} Manager"
  ${EndIf}
!endif

  ${IfNot} ${SectionIsSelected} ${SecShortcuts}
    SetShellVarContext all
    Delete "$SMPROGRAMS\${APP}\${APP}.lnk"
    Delete "$SMPROGRAMS\${APP}\${APP} Manager.lnk"
    Delete "$SMPROGRAMS\${APP}\${APP} (recorder, no window).lnk"
    Delete "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk"
    RMDir "$SMPROGRAMS\${APP}"
  ${EndIf}

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

; Stops the running Manager and recorder before their files are replaced or deleted. First asks them to quit; if a file is still
; locked after ~10 s (for instance an instance in another user's session, which --exit cannot reach) they are ended forcibly.
!macro StopAppFn UN
Function ${UN}StopApp
  ${If} ${FileExists} "$INSTDIR\Manager\${MGR_EXE}"
    nsExec::Exec '"$INSTDIR\Manager\${MGR_EXE}" --exit'
    Pop $0
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\WinFreezeTrace.exe"
    nsExec::Exec '"$INSTDIR\WinFreezeTrace.exe" --exit'
    Pop $0
  ${EndIf}
  StrCpy $1 0
  ${Do}
    StrCpy $2 0
    ${If} ${FileExists} "$INSTDIR\WinFreezeTrace.exe"
      ClearErrors
      FileOpen $3 "$INSTDIR\WinFreezeTrace.exe" a
      ${If} ${Errors}
        StrCpy $2 1
      ${Else}
        FileClose $3
      ${EndIf}
    ${EndIf}
    ${If} ${FileExists} "$INSTDIR\WinFreezeTraceCore.dll"
      ClearErrors
      FileOpen $3 "$INSTDIR\WinFreezeTraceCore.dll" a
      ${If} ${Errors}
        StrCpy $2 1
      ${Else}
        FileClose $3
      ${EndIf}
    ${EndIf}
    ${If} ${FileExists} "$INSTDIR\Manager\${MGR_EXE}"
      ClearErrors
      FileOpen $3 "$INSTDIR\Manager\${MGR_EXE}" a
      ${If} ${Errors}
        StrCpy $2 1
      ${Else}
        FileClose $3
      ${EndIf}
    ${EndIf}
    ${If} $2 == 0
      ${Break}
    ${EndIf}
    ${If} $1 >= 20
      DetailPrint "${APP} is still running; ending it."
      nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM WinFreezeTrace.exe /IM ${MGR_EXE}'
      Pop $0
      Sleep 1500
      ${Break}
    ${EndIf}
    IntOp $1 $1 + 1
    Sleep 500
  ${Loop}
FunctionEnd
!macroend
!insertmacro StopAppFn ""
!insertmacro StopAppFn "un."

; An install of the other architecture (x86 <-> x64) uses another folder and registry view, so it would stay behind as a second
; copy. It is removed first (its reports and settings are kept).
Function CheckOtherArch
!if "${ARCH}" == "x64"
  SetRegView 32
!else
  ${IfNot} ${RunningX64}
    Return
  ${EndIf}
  SetRegView 64
!endif
  ReadRegStr $R2 HKLM "Software\${APP}" "InstallDir"
!if "${ARCH}" == "x64"
  SetRegView 64
  StrCpy $R3 "32-bit (x86)"
!else
  SetRegView 32
  StrCpy $R3 "64-bit (x64)"
!endif
  ${If} $R2 == ""
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$R2\Uninstall.exe"
    Return
  ${EndIf}
  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION "The $R3 version of ${APP} is installed in:$\r$\n$R2$\r$\n$\r$\nIt has to be removed before this version is installed. Your reports and settings are kept.$\r$\n$\r$\nRemove it now?" IDYES +2
    Abort
  ${EndIf}
  ExecWait '"$R2\Uninstall.exe" /S _?=$R2' $0
  Delete "$R2\Uninstall.exe"
  RMDir "$R2"
FunctionEnd

; Installing an older version over a newer one is refused in silent mode (unless /ALLOWDOWNGRADE is given) and confirmed otherwise.
Function CheckDowngrade
  ReadRegStr $0 HKLM "Software\${APP}" "VersionNum"
  ${If} $0 == ""
    ReadRegStr $0 HKLM "${UNINST_KEY}" "DisplayVersion"   ; installs made before VersionNum existed
  ${EndIf}
  ${If} $0 == ""
    Return
  ${EndIf}
  ${VersionCompare} "$0" "${VERSION_NUM}" $1
  ${If} $1 == 1   ; the installed version is newer
    ${GetOptions} $Params "/ALLOWDOWNGRADE" $2
    ${IfNot} ${Errors}
      Return
    ${EndIf}
    ${If} ${Silent}
      SetErrorLevel 5
      Abort
    ${EndIf}
    MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "A newer version ($0) of ${APP} is already installed.$\r$\nThis setup would replace it with ${VERSION}.$\r$\n$\r$\nContinue anyway?" IDYES +2
    Abort
  ${EndIf}
FunctionEnd

; Pre-ticks the components of the previous install, then applies the silent switches:
;   /MANAGER=0|1  /PATH=0|1  /STARTUP=0|1  /SHORTCUTS=0|1  /ALLOWDOWNGRADE
Function ApplyChoices
  ReadRegStr $R4 HKLM "Software\${APP}" "InstallDir"
  ${If} $R4 != ""
    ReadRegDWORD $0 HKLM "Software\${APP}" "StartupAdded"
    ${If} $0 == 1
      !insertmacro SelectSection ${SecStartup}
    ${EndIf}
    ReadRegDWORD $0 HKLM "Software\${APP}" "PathAdded"
    ${If} $0 != 1
      !insertmacro UnselectSection ${SecPath}
    ${EndIf}
    SetShellVarContext all
    ${IfNot} ${FileExists} "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk"
      !insertmacro UnselectSection ${SecShortcuts}
    ${EndIf}
!ifdef HAVE_MANAGER
    ${IfNot} ${FileExists} "$R4\Manager\${MGR_EXE}"
      !insertmacro UnselectSection ${SecManager}
    ${EndIf}
!endif
  ${EndIf}

!ifdef HAVE_MANAGER
  ${GetOptions} $Params "/MANAGER=" $R1
  StrCpy $R1 $R1 1   ; the value is a single digit; GetOptions can return the rest of the command line
  ${If} $R1 == "0"
    !insertmacro UnselectSection ${SecManager}
  ${ElseIf} $R1 == "1"
    !insertmacro SelectSection ${SecManager}
  ${EndIf}
!endif
  ${GetOptions} $Params "/PATH=" $R1
  StrCpy $R1 $R1 1   ; the value is a single digit; GetOptions can return the rest of the command line
  ${If} $R1 == "0"
    !insertmacro UnselectSection ${SecPath}
  ${ElseIf} $R1 == "1"
    !insertmacro SelectSection ${SecPath}
  ${EndIf}
  ${GetOptions} $Params "/STARTUP=" $R1
  StrCpy $R1 $R1 1   ; the value is a single digit; GetOptions can return the rest of the command line
  ${If} $R1 == "0"
    !insertmacro UnselectSection ${SecStartup}
  ${ElseIf} $R1 == "1"
    !insertmacro SelectSection ${SecStartup}
  ${EndIf}
  ${GetOptions} $Params "/SHORTCUTS=" $R1
  StrCpy $R1 $R1 1   ; the value is a single digit; GetOptions can return the rest of the command line
  ${If} $R1 == "0"
    !insertmacro UnselectSection ${SecShortcuts}
  ${ElseIf} $R1 == "1"
    !insertmacro SelectSection ${SecShortcuts}
  ${EndIf}
FunctionEnd

; .onInit uses ${SecManager}, so it must come after the Section definitions.
Function .onInit
!if "${ARCH}" == "x64"
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This is the 64-bit build of ${APP}. Use the x86 installer on 32-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
!endif
  ${GetParameters} $Params
  ; Re-use the folder of the existing install of this architecture, unless /D= gave another one.
  ReadRegStr $R5 HKLM "Software\${APP}" "InstallDir"
!if "${ARCH}" == "x64"
  StrCpy $R6 "$PROGRAMFILES64\${APP}"
!else
  StrCpy $R6 "$PROGRAMFILES\${APP}"
!endif
  ${If} $R5 != ""
  ${AndIf} $INSTDIR == $R6
    StrCpy $INSTDIR $R5
  ${EndIf}
  Call CheckOtherArch
  Call CheckDowngrade
  Call ApplyChoices
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

  Call un.StopApp
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
  !insertmacro RegView64
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

  ; Copies made by install_task.cmd (%LOCALAPPDATA%\WinFreezeTrace\bin) belong to the program, so they go too (the task that ran
  ; them was deleted above). Reports and settings in %LOCALAPPDATA%\WinFreezeTrace are not touched.
  StrCpy $2 0
  uprof_loop:
    EnumRegKey $3 HKLM "${PROFILELIST}" $2
    StrCmp $3 "" uprof_done
    IntOp $2 $2 + 1
    ReadRegStr $4 HKLM "${PROFILELIST}\$3" "ProfileImagePath"
    ExpandEnvStrings $4 $4
    StrCmp $4 "" uprof_loop
    Delete /REBOOTOK "$4\AppData\Local\WinFreezeTrace\bin\WinFreezeTrace.exe"
    Delete /REBOOTOK "$4\AppData\Local\WinFreezeTrace\bin\WinFreezeTraceCore.dll"
    RMDir "$4\AppData\Local\WinFreezeTrace\bin"
    RMDir "$4\AppData\Local\WinFreezeTrace"   ; only succeeds when nothing else (reports, settings) is left in it
    Goto uprof_loop
  uprof_done:
  !insertmacro RegViewRestore

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
