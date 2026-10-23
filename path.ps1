<#
 Adds/removes a directory in the machine-wide PATH (HKLM). Called by the NSIS installer (admin).
 Writes the raw registry value so existing %VARIABLES% in PATH are preserved (REG_EXPAND_SZ).
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('add', 'remove')][string]$Action,
    [Parameter(Mandatory = $true)][string]$Dir
)
$ErrorActionPreference = 'Stop'
$Dir = $Dir.TrimEnd('\')
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $true)
try {
    $raw = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $parts = @($raw -split ';' | Where-Object { $_.Trim() -ne '' })
    $others = @($parts | Where-Object { $_.TrimEnd('\') -ine $Dir })
    if ($Action -eq 'add') { $others += $Dir }
    $new = $others -join ';'
    if ($new -ne $raw) { $key.SetValue('Path', $new, [Microsoft.Win32.RegistryValueKind]::ExpandString) }
} finally { $key.Close() }
