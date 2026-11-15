# Install-Prerequisites.ps1 - one-time setup of the tools needed to build 3DO to PC and converted games.
#
# Installs (only what is missing):
#   * Build Tools for Visual Studio 2022: C++ x64 compiler, Windows SDK, C++ CMake tools (cmake + ninja)
#   * .NET 8 SDK (any SDK 8 or newer is accepted) - only needed for the "3DO to PC.exe" window app
#
# This is the only step that uses the internet. Converting and building games afterwards is fully local.
#
# Usage: double-click Install-Prerequisites.bat, or
#        powershell -ExecutionPolicy Bypass -File Install-Prerequisites.ps1 [-SkipDotNet]

[CmdletBinding()]
param([switch]$SkipDotNet)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Installing Visual Studio components needs administrator rights: relaunch elevated.
if (-not (Test-Admin)) {
    Write-Host 'Administrator rights are needed; Windows will ask for permission...'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($SkipDotNet) { $argList += '-SkipDotNet' }
    try { Start-Process powershell.exe -Verb RunAs -ArgumentList $argList }
    catch { Write-Host 'Permission was not granted; nothing was installed.' -ForegroundColor Red; Read-Host 'Press Enter to close' | Out-Null }
    exit
}

function Step([string]$msg) { Write-Host ''; Write-Host "== $msg" -ForegroundColor Cyan }
function Ok([string]$msg) { Write-Host "OK: $msg" -ForegroundColor Green }

$work = Join-Path $env:TEMP '3DO to PC setup'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$rebootNeeded = $false
$failed = $false

function Get-VsWhere {
    $p = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $p) { return $p } else { return $null }
}

# Components 3DO to PC needs. --includeRecommended adds the Windows SDK and the rest of the C++ basics.
$vsComponents = @(
    'Microsoft.VisualStudio.Workload.VCTools',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.VC.CMake.Project'
)

# An install counts as ready only if cl.exe's toolset AND cmake.exe really exist on disk.
function Find-ReadyVs {
    $vswhere = Get-VsWhere
    if (-not $vswhere) { return $null }
    $paths = & $vswhere -all -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project `
        -property installationPath
    foreach ($p in $paths) {
        $cmake = Join-Path $p 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        $vcvars = Join-Path $p 'VC\Auxiliary\Build\vcvars64.bat'
        if ((Test-Path $cmake) -and (Test-Path $vcvars)) { return $p }
    }
    return $null
}

try {
    # ------------------------------------------------------------ Visual Studio
    Step 'Visual Studio C++ Build Tools'
    $ready = Find-ReadyVs
    if ($ready) {
        Ok "already installed with the needed components: $ready"
    } else {
        $boot = Join-Path $work 'vs_buildtools.exe'
        Write-Host 'Downloading the Visual Studio Build Tools installer...'
        Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile $boot

        $vsArgs = @()
        $vswhere = Get-VsWhere
        $existing = $null
        if ($vswhere) {
            $existing = & $vswhere -products Microsoft.VisualStudio.Product.BuildTools -property installationPath |
                Select-Object -First 1
        }
        if ($existing) {
            Write-Host "Adding components to the existing Build Tools at $existing"
            $vsArgs += @('modify', '--installPath', "`"$existing`"")
        } else {
            Write-Host 'Installing Build Tools for Visual Studio 2022 (several GB; this can take 10-30 minutes)...'
        }
        foreach ($c in $vsComponents) { $vsArgs += @('--add', $c) }
        $vsArgs += @('--includeRecommended', '--passive', '--wait', '--norestart')

        $p = Start-Process -FilePath $boot -ArgumentList $vsArgs -Wait -PassThru
        switch ($p.ExitCode) {
            0       { Ok 'Visual Studio components installed' }
            3010    { Ok 'Visual Studio components installed (restart recommended)'; $rebootNeeded = $true }
            1641    { Ok 'Visual Studio components installed (restart required)'; $rebootNeeded = $true }
            default { throw "Visual Studio installer exited with code $($p.ExitCode)" }
        }
        $ready = Find-ReadyVs
        if (-not $ready) { throw 'Visual Studio installed, but the C++ compiler or CMake tools are still missing.' }
        Ok "C++ compiler and CMake tools ready: $ready"
    }

    # ------------------------------------------------------------ .NET SDK
    Step '.NET SDK (for the 3DO to PC app)'
    if ($SkipDotNet) {
        Write-Host 'Skipped (-SkipDotNet). The command-line converter does not need it.'
    } else {
        function Test-DotNetSdk {
            $dn = Get-Command dotnet -ErrorAction SilentlyContinue
            if (-not $dn) { return $false }
            $sdks = & $dn.Source --list-sdks 2>$null
            foreach ($line in $sdks) { if ($line -match '^(\d+)\.' -and [int]$matches[1] -ge 8) { return $true } }
            return $false
        }
        function Update-Path {
            $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                        [Environment]::GetEnvironmentVariable('Path', 'User')
        }

        if (Test-DotNetSdk) {
            Ok '.NET SDK 8 or newer already installed'
        } else {
            $winget = Get-Command winget -ErrorAction SilentlyContinue
            if ($winget) {
                Write-Host 'Installing the .NET 8 SDK with winget...'
                & $winget.Source install --id Microsoft.DotNet.SDK.8 -e --silent `
                    --accept-source-agreements --accept-package-agreements | Out-Host
            } else {
                Write-Host 'winget not available; using the official dotnet-install script...'
                $script = Join-Path $work 'dotnet-install.ps1'
                Invoke-WebRequest -UseBasicParsing -Uri 'https://dot.net/v1/dotnet-install.ps1' -OutFile $script
                $dir = Join-Path $env:ProgramFiles 'dotnet'
                & $script -Channel 8.0 -InstallDir $dir
                $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
                if (($machinePath -split ';') -notcontains $dir) {
                    [Environment]::SetEnvironmentVariable('Path', "$machinePath;$dir", 'Machine')
                }
            }
            Update-Path
            if (Test-DotNetSdk) { Ok '.NET SDK installed' }
            else { throw '.NET SDK installation did not complete. Install it manually from https://dotnet.microsoft.com/download/dotnet/8.0' }
        }
    }
}
catch {
    Write-Host ''
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Visual Studio installer logs are in $env:TEMP (files named dd_*.log)."
    $failed = $true
}

Write-Host ''
if ($failed) {
    Write-Host 'Setup did not finish. Copy the text in this window and send it to me.' -ForegroundColor Red
} else {
    Write-Host 'All set.' -ForegroundColor Green
    if ($rebootNeeded) { Write-Host 'Restart Windows before building, as the installer recommended.' -ForegroundColor Yellow }
    Write-Host 'Next: double-click Build-App.bat (builds the app), or drag a game onto Convert-3DO.bat.'
    Write-Host 'Open a NEW window for those so they see the updated PATH.'
}
Read-Host 'Press Enter to close' | Out-Null
