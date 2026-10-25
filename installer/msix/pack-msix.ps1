param(
    [Parameter(Mandatory)][string]$Arch,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Dll,
    [Parameter(Mandatory)][string]$ManagerDir,
    [Parameter(Mandatory)][string]$Out,
    [string]$IdentityName = 'TamKungZ.WinFreezeTraceManager',
    [string]$Publisher,
    [string]$PublisherDisplayName = 'TamKungZ_',
    [string]$DisplayName = 'WinFreezeTrace Manager',
    [string]$Pfx,
    [string]$PfxPassword
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

$kit = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\makeappx.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1
if (-not $kit) { throw 'makeappx.exe not found (Windows SDK missing)' }
$makeappx = $kit.FullName
$signtool = Join-Path $kit.DirectoryName 'signtool.exe'

$certSubject = $null
if ($Pfx) {
    $certSubject = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $Pfx, $PfxPassword).Subject
}
if (-not $Publisher) { $Publisher = if ($certSubject) { $certSubject } else { 'CN=TamKungZ_' } }
if ($certSubject -and $certSubject -ne $Publisher) {
    throw "Certificate subject '$certSubject' does not match Publisher '$Publisher'. Do not sign packages that use a Partner Center publisher."
}

$stage = Join-Path ([IO.Path]::GetTempPath()) "msix-$Arch"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'Assets') | Out-Null

Copy-Item $Exe, $Dll, "$root\LICENSE", "$root\TERMS.txt", "$root\PRIVACY.txt", "$root\README.md", "$root\CHANGELOG.md" $stage
Copy-Item $ManagerDir (Join-Path $stage 'Manager') -Recurse

Copy-Item "$PSScriptRoot\Assets\*.png" (Join-Path $stage 'Assets')

$manifest = (Get-Content "$PSScriptRoot\AppxManifest.xml" -Raw).
    Replace('@IDENTITY_NAME@', [Security.SecurityElement]::Escape($IdentityName)).
    Replace('@PUBLISHER@', [Security.SecurityElement]::Escape($Publisher)).
    Replace('@PUBLISHER_DISPLAY_NAME@', [Security.SecurityElement]::Escape($PublisherDisplayName)).
    Replace('@DISPLAY_NAME@', [Security.SecurityElement]::Escape($DisplayName)).
    Replace('@VERSION@', $Version).
    Replace('@ARCH@', $Arch)
[IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'), $manifest, (New-Object System.Text.UTF8Encoding $false))

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
& $makeappx pack /d $stage /p $Out /o
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed ($LASTEXITCODE)" }

if ($Pfx) {
    & $signtool sign /fd SHA256 /f $Pfx /p $PfxPassword /tr http://timestamp.digicert.com /td SHA256 $Out
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
}