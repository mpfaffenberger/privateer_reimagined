# build_and_run.ps1 — configure + build new_privateer on Windows (MSVC + Ninja)
# Run from the repo root: powershell -ExecutionPolicy Bypass -File scripts\build_and_run.ps1

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# --- 1. Enter VS Developer Shell (gives us cl.exe, link.exe, Win SDK) ---
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "VS Build Tools not found" }
Write-Host "[build] VS: $vsPath" -ForegroundColor Cyan
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

# --- 2. Download sokol-shdc.exe if missing ---
$shdc = Join-Path $repoRoot 'third_party\bin\sokol-shdc.exe'
if (-not (Test-Path $shdc)) {
    Write-Host "[build] Downloading sokol-shdc.exe..." -ForegroundColor Yellow
    $url = 'https://github.com/floooh/sokol-tools-bin/raw/master/bin/win32/sokol-shdc.exe'
    Invoke-WebRequest -Uri $url -OutFile $shdc
}
Write-Host "[build] shdc: $((Get-Item $shdc).Length) bytes" -ForegroundColor Green

# --- 3. Configure ---
$buildDir = Join-Path $repoRoot 'build'
Write-Host "[build] CMake configure..." -ForegroundColor Cyan
cmake -B $buildDir -S $repoRoot -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# --- 4. Build ---
Write-Host "[build] Building new_privateer..." -ForegroundColor Cyan
cmake --build $buildDir --target new_privateer

$exe = Join-Path $buildDir 'new_privateer.exe'
if (Test-Path $exe) {
    $sizeMB = [math]::Round((Get-Item $exe).Length / 1MB, 1)
    Write-Host "[build] SUCCESS! $exe ($sizeMB MB)" -ForegroundColor Green
} else {
    throw "Build failed — exe not found at $exe"
}
