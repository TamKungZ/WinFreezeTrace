@echo off
setlocal EnableExtensions

set "TASK_NAME=WinFreezeTrace"
set "APP_ROOT=%LOCALAPPDATA%\WinFreezeTrace"
set "INSTALL_DIR=%APP_ROOT%\bin"
set "TARGET=%INSTALL_DIR%\WinFreezeTrace.exe"
set "FAILED=0"

if not defined LOCALAPPDATA (
  echo ERROR: LOCALAPPDATA is not available for this user.
  exit /b 1
)

echo Stopping WinFreezeTrace...
schtasks.exe /End /TN "%TASK_NAME%" >nul 2>nul
taskkill.exe /F /IM WinFreezeTrace.exe >nul 2>nul
timeout.exe /T 1 /NOBREAK >nul

tasklist.exe /FI "IMAGENAME eq WinFreezeTrace.exe" /NH 2>nul | findstr.exe /I /C:"WinFreezeTrace.exe" >nul
if not errorlevel 1 (
  echo ERROR: WinFreezeTrace.exe is still running. Rerun with sufficient permission.
  set "FAILED=1"
)

schtasks.exe /Query /TN "%TASK_NAME%" >nul 2>nul
if not errorlevel 1 (
  schtasks.exe /Delete /F /TN "%TASK_NAME%" >nul
  if errorlevel 1 (
    echo ERROR: Could not remove scheduled task "%TASK_NAME%".
    set "FAILED=1"
  )
)

if exist "%TARGET%" del /F /Q "%TARGET%" >nul 2>nul
if exist "%TARGET%" (
  echo ERROR: Could not remove "%TARGET%".
  set "FAILED=1"
)

if exist "%INSTALL_DIR%" rd "%INSTALL_DIR%" >nul 2>nul

if /I "%~1"=="--purge" (
  echo Removing reports and all remaining WinFreezeTrace user data...
  powershell.exe -NoProfile -Command "$base=[IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\'); $target=[IO.Path]::GetFullPath([IO.Path]::Combine($base,'WinFreezeTrace')).TrimEnd('\'); if ([IO.Path]::GetDirectoryName($target).TrimEnd('\') -ne $base) { throw 'Refusing unexpected purge path' }; if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }"
  if errorlevel 1 (
    echo ERROR: Could not purge "%APP_ROOT%".
    set "FAILED=1"
  )
) else (
  if exist "%APP_ROOT%\reports" echo Reports were preserved in "%APP_ROOT%\reports".
  echo Run uninstall_task.cmd --purge to also delete reports and remaining user data.
)

if "%FAILED%"=="1" exit /b 1
echo WinFreezeTrace was uninstalled successfully.
exit /b 0
