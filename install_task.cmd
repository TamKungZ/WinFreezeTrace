@echo off
setlocal EnableExtensions
rem Usage: install_task.cmd [WinFreezeTrace options]   e.g.  install_task.cmd --redact --max-reports 20

set "TASK_NAME=WinFreezeTrace"
set "INSTALL_DIR=%LOCALAPPDATA%\WinFreezeTrace\bin"
set "TARGET=%INSTALL_DIR%\WinFreezeTrace.exe"
set "SOURCE=%~dp0WinFreezeTrace.exe"
set "DLL_NAME=WinFreezeTraceCore.dll"
set "SOURCE_DLL=%~dp0%DLL_NAME%"
set "ME=%USERDOMAIN%\%USERNAME%"

if not defined LOCALAPPDATA (
  echo ERROR: LOCALAPPDATA is not available for this user.
  exit /b 1
)

if not exist "%SOURCE%" set "SOURCE=%~dp0build\Release\WinFreezeTrace.exe"
if not exist "%SOURCE_DLL%" set "SOURCE_DLL=%~dp0build\Release\%DLL_NAME%"
if not exist "%SOURCE%" (
  echo ERROR: WinFreezeTrace.exe was not found.
  echo Place it next to this script or build the Release configuration first.
  exit /b 1
)
if not exist "%SOURCE_DLL%" (
  echo ERROR: %DLL_NAME% was not found.
  echo It must be next to WinFreezeTrace.exe ^(the exe only loads the engine from this DLL^).
  exit /b 1
)

where schtasks.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: schtasks.exe is not available.
  exit /b 1
)

echo Stopping an existing WinFreezeTrace task and process of this user...
schtasks.exe /End /TN "%TASK_NAME%" >nul 2>nul
rem Ask a running recorder to quit cleanly first (it may be in the middle of writing a report).
if exist "%TARGET%" "%TARGET%" --exit >nul 2>nul
rem Only this user's instance: never kill another user's recorder on a shared PC.
taskkill.exe /F /FI "USERNAME eq %ME%" /IM WinFreezeTrace.exe >nul 2>nul
timeout.exe /T 1 /NOBREAK >nul

tasklist.exe /FI "IMAGENAME eq WinFreezeTrace.exe" /FI "USERNAME eq %ME%" /NH 2>nul | findstr.exe /I /C:"WinFreezeTrace.exe" >nul
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
if /I not "%SOURCE_DLL%"=="%INSTALL_DIR%\%DLL_NAME%" (
  copy /Y "%SOURCE_DLL%" "%INSTALL_DIR%\%DLL_NAME%" >nul
  if errorlevel 1 (
    echo ERROR: Could not copy %DLL_NAME% to "%INSTALL_DIR%".
    exit /b 1
  )
)

rem Extra options for the recorder are passed to the task through an environment variable
rem (never spliced into a command line).
set "WFT_ARGS=%*"
set "REGISTERED="
where powershell.exe >nul 2>nul
if not errorlevel 1 (
  rem ExecutionPolicy Bypass applies to this single child process only; it is needed because a script
  rem extracted from a downloaded zip carries the internet-zone mark.
  powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0register_task.ps1" -Exe "%TARGET%" -TaskName "%TASK_NAME%"
  if not errorlevel 1 set "REGISTERED=1"
)

if not defined REGISTERED (
  echo WARNING: Could not register the task through PowerShell; falling back to schtasks.exe.
  echo          Windows will apply its defaults: the task stops after 3 days and does not run on battery.
  schtasks.exe /Create /F /SC ONLOGON /RL LIMITED /TN "%TASK_NAME%" /TR "\"%TARGET%\" %WFT_ARGS%" >nul
  if errorlevel 1 (
    echo ERROR: Could not create the scheduled task.
    exit /b 1
  )
)

schtasks.exe /Run /TN "%TASK_NAME%" >nul
if errorlevel 1 (
  echo WARNING: The task was installed but could not be started now.
  echo It will be started automatically at the next user logon.
  exit /b 0
)

echo WinFreezeTrace installed successfully.
echo Executable: "%TARGET%"
echo Task: %TASK_NAME% ^(current user, LIMITED, starts at logon^)
echo The recorder has no tray icon: use WinFreezeTrace Manager ^(a separate program^) to turn monitoring on/off.
echo Log: "%LOCALAPPDATA%\WinFreezeTrace\winfreezetrace.log"
echo Reports: "%LOCALAPPDATA%\WinFreezeTrace\reports"
exit /b 0
