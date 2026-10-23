<#
 Registers the WinFreezeTrace scheduled task for the CURRENT user.
 Called by install_task.cmd. Arguments for the recorder are read from $env:WFT_ARGS.

 Why not "schtasks /Create": tasks created that way inherit Windows' defaults
 ("stop after 3 days", "do not start on battery", "stop on battery"), which would
 silently kill a long-running recorder. This script sets those explicitly.
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$TaskName = 'WinFreezeTrace'
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "Executable not found: $Exe" }
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

$actionArgs = @{ Execute = $Exe }
if ($env:WFT_ARGS -and $env:WFT_ARGS.Trim().Length -gt 0) { $actionArgs.Argument = $env:WFT_ARGS.Trim() }
$action    = New-ScheduledTaskAction @actionArgs
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $me
$principal = New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Limited
$settings  = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Force | Out-Null
Write-Output "Registered task '$TaskName' for $me (limited rights, no time limit, runs on battery)."
