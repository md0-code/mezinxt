<#
.SYNOPSIS
    Build a Windows release and make its archive in dist/.

.DESCRIPTION
    Builds mezinxt.exe in Release (SDL2 and the C runtime linked statically)
    through tools/build.ps1 and packs it with tools/package.py into
    dist/releases/MezinXT-<version>-windows-<arch>.zip: the program, mezinxt.yaml (the
    manual disk in drive A, the composite monitor, the CRT look), README.md, README.ro.md, docs/ (LICENSE,
    THIRD_PARTY.md, CHANGELOG.md, NOTES.md, NOTES.ro.md), roms/ (the FEPER BIOSes and character ROM, from tools/fetch_roms.py)
    and disks/manual.img (built from src/manual-disk/). dist/releases/ holds the current version only. The file
    list is fixed in tools/package.py and checked against the finished
    archive: anything unexpected or missing fails the build.

    -Arch x64 (default) runs the tests (ctest) and, when the archive is made, the
    release check (tests/verify.py release: a fresh extraction starts the manual
    disk headless), nothing more. dist/ then doubles as a run folder: the release files are
    copied over it, and dist/mezinxt.yaml and every other file there stay.

    -Arch arm64 cross-compiles for Windows on ARM64 (it cannot be run here) into
    build/dist-arm64 and adds dist/releases/MezinXT-<version>-windows-arm64.zip; the
    x64 run folder is not touched. It needs the Visual Studio component
    "MSVC Build Tools for ARM64" (exit code 3 without it).

.EXAMPLE
    pwsh -File tools/build_dist.ps1                  # build, test, pack, refresh dist/
    pwsh -File tools/build_dist.ps1 -NoTest          # without the tests
    pwsh -File tools/build_dist.ps1 -Arch arm64      # the ARM64 archive
#>
[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",
    [string]$BuildDir = "",
    [switch]$NoTest,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$name = "MezinXT"
$program = "mezinxt.exe"
if (-not $BuildDir) { $BuildDir = Join-Path $root "build/dist-$Arch" }
$version = (Get-Content (Join-Path $root "src/VERSION") -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "src/VERSION must hold MAJOR.MINOR.PATCH, not '$version'" }
$platform = "windows-$Arch"
Write-Host "$name $version ($platform)"

if ($Arch -eq "x64") {
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path -ieq (Join-Path $root "dist\$program"))
    }
    if ($running) { throw "$name is running from dist/ (process $($running.Id -join ', ')): close it and run this again." }
}

# --- build and test ---------------------------------------------------------
$buildArgs = @{ Arch = $Arch; Config = "Release"; BuildDir = $BuildDir; StaticRuntime = $true }
if ($Clean) { $buildArgs.Clean = $true }
if ($NoTest -or $Arch -ne "x64") { $buildArgs.NoTest = $true }
& (Join-Path $root "tools/build.ps1") @buildArgs
if ($LASTEXITCODE -eq 3) { Write-Host "No archive for ${platform}: the ARM64 toolset is missing." -ForegroundColor Yellow; exit 3 }
$exe = Join-Path $BuildDir $program
if (-not (Test-Path $exe)) { throw "the build produced no $exe" }
$runTests = -not $NoTest -and $Arch -eq "x64"

# --- the archive, and for x64 the run folder --------------------------------
$distRoot = Join-Path $root "dist"
$packArgs = @("windows-$Arch", $exe, "--out", (Join-Path $distRoot "releases"))
if ($Arch -eq "x64") { $packArgs += @("--run-folder", $distRoot) }
& python (Join-Path $root "tools/package.py") @packArgs
if ($LASTEXITCODE -ne 0) { throw "tools/package.py failed ($LASTEXITCODE)" }

# --- the exit check: a fresh extraction of the archive starts the manual disk
if ($runTests) {
    $env:MEZINXT_BUILD = $BuildDir          # the check starts from the program that was packed
    & python (Join-Path $root "tests/verify.py") release
    if ($LASTEXITCODE -ne 0) { throw "the release check failed ($LASTEXITCODE)" }
}
Write-Host "OK: dist/releases/$name-$version-$platform.zip" -ForegroundColor Green
