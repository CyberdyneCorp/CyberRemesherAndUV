# UNVERIFIED: requires a Windows runner with MSVC, CMake and (for signing)
# signtool + a code-signing certificate. Cannot run in headless Linux CI;
# exercised by .github/workflows/release.yml on the windows lane.
#
# Builds the CyberRemesher CLI + desktop shell and produces a versioned zip
# archive (build-and-packaging spec: "Windows (zip and installer)").
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir "..\..")

# Single version source: read project(... VERSION ...) from root CMakeLists.
$cmake = Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw
if ($cmake -notmatch 'VERSION\s+(\d+\.\d+\.\d+)') {
    throw "Could not read project VERSION from CMakeLists.txt"
}
$Version = $Matches[1]
$ZipName = "cyberremesh-$Version-windows-x64.zip"

Write-Host "==> Building CyberRemesher $Version (Windows, x64)"
# cpu-headless, not windows-cuda: the CUDA preset requires nvcc, which the
# GitHub windows-latest runner does not ship, so configure failed outright.
# The packaged artifact is the headless CLI + C ABI, neither of which needs CUDA.
cmake --preset cpu-headless
cmake --build --preset cpu-headless

$Dist  = Join-Path $RepoRoot "dist"
$Stage = Join-Path $Dist "windows\stage"
Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

Copy-Item (Join-Path $RepoRoot "build\cpu-headless\apps\cli\cyberremesh.exe") $Stage
Copy-Item (Join-Path $RepoRoot "build\cpu-headless\apps\desktop\CyberRemesher.exe") $Stage -ErrorAction SilentlyContinue
Copy-Item (Join-Path $RepoRoot "LICENSE") $Stage
Copy-Item (Join-Path $RepoRoot "README.md") $Stage

# Optional Authenticode signing (placeholder secrets).
$CertPath = $env:WINDOWS_CERT_PFX
$CertPass = $env:WINDOWS_CERT_PASSWORD
if ($CertPath -and (Test-Path $CertPath)) {
    Write-Host "==> Signing binaries"
    Get-ChildItem $Stage -Filter *.exe | ForEach-Object {
        & signtool sign /f $CertPath /p $CertPass /tr http://timestamp.digicert.com /td sha256 /fd sha256 $_.FullName
    }
} else {
    Write-Host "==> No WINDOWS_CERT_PFX set; producing unsigned zip"
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$ZipPath = Join-Path $Dist $ZipName
Remove-Item -Force $ZipPath -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath

Write-Host $ZipPath
