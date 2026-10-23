
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

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
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
!if "${ARCH}" == "x64"
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This is the 64-bit build of ${APP}. Use the x86 installer on 32-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
!endif
FunctionEnd

Function un.onInit
!if "${ARCH}" == "x64"
  SetRegView 64
!endif
FunctionEnd

Section "${APP} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${EXE}"
  File "${DLL}"
  File "${SRCDIR}\LICENSE"
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
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "TamKungZ"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\WinFreezeTrace.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/TamKungZ/WinFreezeTrace"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

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

Section "Start Menu shortcuts" SecShortcuts
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortcut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\WinFreezeTrace.exe"
  CreateShortcut "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

LangString DESC_Main ${LANG_ENGLISH} "The recorder (WinFreezeTrace.exe + WinFreezeTraceCore.dll), documentation and the optional logon-task scripts."
LangString DESC_Path ${LANG_ENGLISH} "Adds the install folder to the machine PATH so WinFreezeTrace can be called by name."
LangString DESC_SM ${LANG_ENGLISH} "Start Menu entries for all users."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_Main)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_Path)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} $(DESC_SM)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  SetShellVarContext all

  nsExec::Exec '"$SYSDIR\schtasks.exe" /End /TN "${APP}"'
  Pop $0
  nsExec::Exec '"$SYSDIR\schtasks.exe" /Delete /F /TN "${APP}"'
  Pop $0

  ReadRegDWORD $1 HKLM "Software\${APP}" "PathAdded"
  ${If} $1 == 1
    nsExec::Exec '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\path.ps1" -Action remove -Dir "$INSTDIR"'
    Pop $0
    System::Call 'user32::SendMessageTimeoutW(i 0xffff, i 0x1A, i 0, w "Environment", i 2, i 5000, *i .r0)'
  ${EndIf}

  Delete /REBOOTOK "$INSTDIR\WinFreezeTrace.exe"
  Delete /REBOOTOK "$INSTDIR\WinFreezeTraceCore.dll"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\CHANGELOG.md"
  Delete "$INSTDIR\install_task.cmd"
  Delete "$INSTDIR\uninstall_task.cmd"
  Delete "$INSTDIR\register_task.ps1"
  Delete "$INSTDIR\path.ps1"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APP}\${APP}.lnk"
  Delete "$SMPROGRAMS\${APP}\Uninstall ${APP}.lnk"
  RMDir "$SMPROGRAMS\${APP}"

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\${APP}"
SectionEnd
