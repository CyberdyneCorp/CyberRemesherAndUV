# UNVERIFIED: requires a Windows runner with WiX Toolset (candle/light) or
# Inno Setup, plus MSVC build outputs from build_zip.ps1. Cannot run in
# headless Linux CI; exercised by release.yml on the windows lane.
#
# Wraps the staged Windows build into a versioned .msi installer
# (build-and-packaging spec: "Windows (zip and installer)").
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir "..\..")

$cmake = Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw
if ($cmake -notmatch 'VERSION\s+(\d+\.\d+\.\d+)') {
    throw "Could not read project VERSION from CMakeLists.txt"
}
$Version = $Matches[1]
$MsiName = "cyberremesh-$Version-windows-x64.msi"

$Dist  = Join-Path $RepoRoot "dist"
$Stage = Join-Path $Dist "windows\stage"
if (-not (Test-Path $Stage)) {
    throw "Staged build not found at $Stage; run build_zip.ps1 first"
}

$MsiPath = Join-Path $Dist $MsiName
Write-Host "==> Building installer $MsiName with WiX"

# WiX authoring lives alongside this script; -dVersion feeds the single source.
& candle.exe -nologo -arch x64 -dVersion=$Version -dStageDir=$Stage `
    (Join-Path $ScriptDir "installer.wxs") -out (Join-Path $Dist "installer.wixobj")
& light.exe -nologo -ext WixUIExtension `
    (Join-Path $Dist "installer.wixobj") -out $MsiPath

$CertPath = $env:WINDOWS_CERT_PFX
if ($CertPath -and (Test-Path $CertPath)) {
    & signtool sign /f $CertPath /p $env:WINDOWS_CERT_PASSWORD /tr http://timestamp.digicert.com /td sha256 /fd sha256 $MsiPath
}

Write-Host $MsiPath
