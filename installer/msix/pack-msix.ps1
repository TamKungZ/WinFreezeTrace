param(
    [Parameter(Mandatory)][string]$Arch,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Dll,
    [Parameter(Mandatory)][string]$ManagerDir,
    [Parameter(Mandatory)][string]$Out,
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

$publisher = 'CN=TamKungZ_'
if ($Pfx) {
    $publisher = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $Pfx, $PfxPassword).Subject
}

$stage = Join-Path ([IO.Path]::GetTempPath()) "msix-$Arch"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'Assets') | Out-Null

Copy-Item $Exe, $Dll, "$root\LICENSE", "$root\TERMS.txt", "$root\PRIVACY.txt", "$root\README.md", "$root\CHANGELOG.md" $stage
Copy-Item $ManagerDir (Join-Path $stage 'Manager') -Recurse

Add-Type -AssemblyName System.Drawing
$icon = New-Object System.Drawing.Icon "$root\assets\winfreezetrace.ico", 256, 256
$source = $icon.ToBitmap()
foreach ($logo in @{ 'StoreLogo' = 50; 'Square44x44Logo' = 44; 'Square150x150Logo' = 150 }.GetEnumerator()) {
    $size = $logo.Value
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($source, 0, 0, $size, $size)
    $g.Dispose()
    $bmp.Save((Join-Path $stage "Assets\$($logo.Key).png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
$source.Dispose()
$icon.Dispose()

$manifest = (Get-Content "$PSScriptRoot\AppxManifest.xml" -Raw).
    Replace('@PUBLISHER@', [Security.SecurityElement]::Escape($publisher)).
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