<#
.SYNOPSIS
  Installs the tools needed to develop the Windows port of Fukami on a fresh Windows 10/11 machine.

.DESCRIPTION
  Uses winget. Safe to re-run: packages that are already installed are skipped.
  Run from an elevated PowerShell (the Visual Studio Build Tools installer needs it):

    Set-ExecutionPolicy -Scope Process Bypass
    .\scripts\setup_windows_dev.ps1

  What it installs (see BUILDING.md section 0 for the macOS equivalent):
    Git, Python 3, CMake, Ninja, LLVM/clang, 7-Zip, Vulkan SDK,
    Visual Studio 2022 Build Tools (C++ workload, clang-cl, Windows SDK, CMake tools),
    chdman (MAME, for making the CHD), and optionally Qt 6 (-WithQt).

  It does NOT install game data. You still need your own Ridge Racer V (USA) disc image.

.PARAMETER WithQt
  Also install Qt 6 for MSVC through aqtinstall (PCSX2's bridge links Qt on macOS; whether the Windows bridge
  needs it is not known yet, so it is opt-in).

.PARAMETER QtVersion
  Qt version for -WithQt. Default 6.8.3.

.PARAMETER Arm64
  Also add the MSVC ARM64 toolchain (only useful on Windows-on-ARM machines).
#>
[CmdletBinding()]
param(
    [switch]$WithQt,
    [string]$QtVersion = '6.8.3',
    [switch]$Arm64
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) { throw 'Run this script from an elevated (Administrator) PowerShell.' }
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget not found. Install "App Installer" from the Microsoft Store (or update Windows) and re-run.'
}

function Install-Winget([string]$Id, [string]$Extra = '') {
    Write-Host "==> $Id" -ForegroundColor Cyan
    $listed = winget list --id $Id --exact --accept-source-agreements 2>$null | Out-String
    if ($listed -match [regex]::Escape($Id)) { Write-Host '    already installed'; return }
    $args = @('install', '--id', $Id, '--exact', '--silent',
              '--accept-package-agreements', '--accept-source-agreements')
    if ($Extra) { $args += @('--override', $Extra) }
    winget @args
    if ($LASTEXITCODE -ne 0) { Write-Warning "winget install $Id exited with $LASTEXITCODE" }
}

# --- core tools ---------------------------------------------------------------------------------------------
Install-Winget 'Git.Git'
Install-Winget 'Python.Python.3.12'
Install-Winget 'Kitware.CMake'
Install-Winget 'Ninja-build.Ninja'
Install-Winget 'LLVM.LLVM'
Install-Winget '7zip.7zip'
Install-Winget 'KhronosGroup.VulkanSDK'

# --- Visual Studio 2022 Build Tools: MSVC, clang-cl, Windows SDK, CMake integration ---------------------------
$vsComponents = @(
    'Microsoft.VisualStudio.Workload.VCTools',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
    'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset',
    'Microsoft.VisualStudio.Component.Windows11SDK.22621',
    'Microsoft.VisualStudio.Component.VC.CMake.Project',
    'Microsoft.VisualStudio.Component.VC.ATL'
)
if ($Arm64) { $vsComponents += 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' }
$override = '--wait --quiet --norestart ' + (($vsComponents | ForEach-Object { "--add $_" }) -join ' ')
Install-Winget 'Microsoft.VisualStudio.2022.BuildTools' $override

# --- chdman (ships with MAME); used to turn your disc dump into a CHD -----------------------------------------
Install-Winget 'MAMEDev.MAME'

# --- git / filesystem settings that matter for this repo -------------------------------------------------------
Write-Host '==> git and filesystem settings' -ForegroundColor Cyan
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) { $git = Get-Command "$env:ProgramFiles\Git\cmd\git.exe" -ErrorAction SilentlyContinue }
if ($git) {
    & $git.Source config --global core.longpaths true
    & $git.Source config --global core.autocrlf input   # generated-code hashes are byte-exact: no CRLF rewriting
} else {
    Write-Warning 'git not on PATH yet; open a new shell and run: git config --global core.longpaths true; git config --global core.autocrlf input'
}
New-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled `
    -PropertyType DWord -Value 1 -Force | Out-Null

# --- optional Qt ----------------------------------------------------------------------------------------------
if ($WithQt) {
    Write-Host "==> Qt $QtVersion (aqtinstall)" -ForegroundColor Cyan
    $py = (Get-Command py -ErrorAction SilentlyContinue)
    if (-not $py) { Write-Warning 'Python launcher not on PATH yet; open a new shell and re-run with -WithQt.' }
    else {
        & py -3.12 -m pip install --user --upgrade aqtinstall
        & py -3.12 -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O C:\Qt
    }
}

# --- report ---------------------------------------------------------------------------------------------------
Write-Host "`nDone. Open a NEW terminal (so PATH refreshes) and check:" -ForegroundColor Green
Write-Host '  git --version; py -3.12 --version; cmake --version; ninja --version; clang --version; chdman'
Write-Host '  & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath'
Write-Host "`nBuild from the 'x64 Native Tools Command Prompt for VS 2022' (or run vcvars64.bat) so cl/clang-cl are on PATH."
Write-Host 'Note: the existing build scripts are zsh (scripts/build_fukami_runtime.sh); on Windows use WSL2 for those,'
Write-Host 'or a Windows-native script once the port has one. A reboot may be needed after the Build Tools install.'
