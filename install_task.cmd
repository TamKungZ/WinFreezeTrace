@echo off
setlocal
set "EXE=%~dp0WinFreezeTrace.exe"
if not exist "%EXE%" (
  echo WinFreezeTrace.exe not found next to this script.
  exit /b 1
)
schtasks /Create /F /SC ONLOGON /RL LIMITED /TN "WinFreezeTrace" /TR "\"%EXE%\" --background"
if errorlevel 1 exit /b 1
echo Installed. WinFreezeTrace will start at logon with normal user privileges.
