@echo off
setlocal EnableExtensions

set "TASK_NAME=WinFreezeTrace"
set "INSTALL_DIR=%LOCALAPPDATA%\WinFreezeTrace\bin"
set "TARGET=%INSTALL_DIR%\WinFreezeTrace.exe"
set "SOURCE=%~dp0WinFreezeTrace.exe"

if not defined LOCALAPPDATA (
  echo ERROR: LOCALAPPDATA is not available for this user.
  exit /b 1
)

if not exist "%SOURCE%" set "SOURCE=%~dp0build\Release\WinFreezeTrace.exe"
if not exist "%SOURCE%" (
  echo ERROR: WinFreezeTrace.exe was not found.
  echo Place it next to this script or build the Release configuration first.
  exit /b 1
)

where schtasks.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: schtasks.exe is not available.
  exit /b 1
)

echo Stopping an existing WinFreezeTrace task and process...
schtasks.exe /End /TN "%TASK_NAME%" >nul 2>nul
taskkill.exe /F /IM WinFreezeTrace.exe >nul 2>nul
timeout.exe /T 1 /NOBREAK >nul

tasklist.exe /FI "IMAGENAME eq WinFreezeTrace.exe" /NH 2>nul | findstr.exe /I /C:"WinFreezeTrace.exe" >nul
if not errorlevel 1 (
  echo ERROR: WinFreezeTrace.exe is still running. Close it or rerun this script with sufficient permission.
  exit /b 1
)

if not exist "%INSTALL_DIR%" (
  mkdir "%INSTALL_DIR%"
  if errorlevel 1 (
    echo ERROR: Could not create "%INSTALL_DIR%".
    exit /b 1
  )
)

if /I not "%SOURCE%"=="%TARGET%" (
  copy /Y "%SOURCE%" "%TARGET%" >nul
  if errorlevel 1 (
    echo ERROR: Could not copy WinFreezeTrace.exe to "%INSTALL_DIR%".
    exit /b 1
  )
)

schtasks.exe /Create /F /SC ONLOGON /RL LIMITED /TN "%TASK_NAME%" /TR "\"%TARGET%\" --background" >nul
if errorlevel 1 (
  echo ERROR: Could not create the scheduled task.
  exit /b 1
)

schtasks.exe /Run /TN "%TASK_NAME%" >nul
if errorlevel 1 (
  echo WARNING: The task was installed but could not be started now.
  echo It will be started automatically at the next user logon.
  exit /b 0
)

echo WinFreezeTrace 0.3.0 installed successfully.
echo Executable: "%TARGET%"
echo Task: %TASK_NAME% ^(current user, LIMITED, starts at logon^)
exit /b 0
