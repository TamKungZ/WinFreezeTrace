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

# The Manager is a WinUI 3 app. WinUI resolves ms-appx:/// (App.xbf, the XAML pages) and resources.pri against the PACKAGE ROOT,
# not against the folder of the exe, so the Manager must sit at the root. In a subfolder it starts fine unpackaged (NSIS, zip)
# but exits during launch inside the package (Application Error 1000, 0xc000027b in Microsoft.UI.Xaml.dll).
Copy-Item "$ManagerDir\*" $stage -Recurse -Force
foreach ($n in 'WinFreezeTrace.exe', 'WinFreezeTraceCore.dll') {
    if (Test-Path (Join-Path $stage $n)) { throw "The Manager output already contains $n; it would be overwritten by the recorder." }
}
Copy-Item $Exe, $Dll, "$root\LICENSE", "$root\TERMS.txt", "$root\PRIVACY.txt", "$root\README.md", "$root\CHANGELOG.md" $stage -Force

if (-not (Test-Path (Join-Path $stage 'WinFreezeTraceManager.exe'))) { throw 'WinFreezeTraceManager.exe is missing from the Manager output.' }
# A packaged app reads its resource index from <package root>\resources.pri. The unpackaged build names it after the exe.
if (-not (Test-Path (Join-Path $stage 'resources.pri'))) {
    $own = Join-Path $stage 'WinFreezeTraceManager.pri'
    if (-not (Test-Path $own)) { throw 'The Manager output has no .pri resource index (resources.pri or WinFreezeTraceManager.pri); the package would not start.' }
    Rename-Item $own 'resources.pri'
}
if (-not (Test-Path (Join-Path $stage 'App.xbf'))) { Write-Warning 'App.xbf was not found at the package root; check that the Manager output includes the compiled XAML (.xbf) files.' }

Copy-Item "$PSScriptRoot\Assets\*.png" (Join-Path $stage 'Assets')

$manifest = (Get-Content "$PSScriptRoot\AppxManifest.xml" -Raw).
    Replace('@IDENTITY_NAME@', [Security.SecurityElement]::Escape($IdentityName)).
    Replace('@PUBLISHER@', [Security.SecurityElement]::Escape($Publisher)).
    Replace('@PUBLISHER_DISPLAY_NAME@', [Security.SecurityElement]::Escape($PublisherDisplayName)).
    Replace('@DISPLAY_NAME@', [Security.SecurityElement]::Escape($DisplayName)).
    Replace('@VERSION@', $Version).
    Replace('@ARCH@', $Arch)
[IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'), $manifest, (New-Object System.Text.UTF8Encoding $false))

# Windows resolves every manifest logo through resources.pri when the package has one. The WinUI build's PRI only indexes the files
# that build knew about, so the Assets\*.png copied above (scale-*, targetsize-*, targetsize-*_altform-unplated) are invisible to it
# and the shell falls back to the plain Square44x44Logo.png and draws a grey/black plate behind it (Task Manager, taskbar, Start).
# Re-index the staged folder so those qualifiers end up in resources.pri. The default config's PRI indexer keeps the WinUI entries.
$makepri = Join-Path $kit.DirectoryName 'makepri.exe'
if (-not (Test-Path $makepri)) { throw 'makepri.exe not found (Windows SDK missing)' }
$tmp = [IO.Path]::GetTempPath()
$priCfg  = Join-Path $tmp "priconfig-$Arch.xml"
$priNew  = Join-Path $tmp "resources-$Arch.pri"
$priDump = Join-Path $tmp "resources-$Arch.xml"
& $makepri createconfig /cf $priCfg /dq en-US /pv 10.0.0 /o
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed ($LASTEXITCODE)" }
& $makepri new /pr $stage /cf $priCfg /mn (Join-Path $stage 'AppxManifest.xml') /of $priNew /o
if ($LASTEXITCODE -ne 0) { throw "makepri new failed ($LASTEXITCODE)" }
Copy-Item $priNew (Join-Path $stage 'resources.pri') -Force
& $makepri dump /if (Join-Path $stage 'resources.pri') /of $priDump /dt detailed /o | Out-Null
if (-not (Select-String -Path $priDump -Pattern 'unplated' -Quiet)) {
    Write-Warning 'resources.pri has no altform-unplated entries; the app icon will get a plate in Task Manager/taskbar.'
}

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
& $makeappx pack /d $stage /p $Out /o
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed ($LASTEXITCODE)" }
Write-Host "Package layout: $stage"
Write-Host "Test it before uploading (Developer Mode on, no signing needed): Add-AppxPackage -Register '$stage\AppxManifest.xml'"

if ($Pfx) {
    & $signtool sign /fd SHA256 /f $Pfx /p $PfxPassword /tr http://timestamp.digicert.com /td SHA256 $Out
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
}