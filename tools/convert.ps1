# convert.ps1 - drag-and-drop driver for 3DO to PC (called by Convert-3DO.bat).
#
#   1. finds Visual Studio / Build Tools (C++) on this PC
#   2. builds the converter (first run only, then incremental)
#   3. converts each dropped disc image or game folder into games\<Name>\
#   4. runs the game, then shows and highlights tdort.log
#
# Dropping the same game again reuses games\<Name>\interp_hits.txt from the
# last run, so code that was interpreted gets recompiled.

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Inputs)

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot

function Done([int]$code) {
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
    exit $code
}
function Step([string]$msg) { Write-Host ''; Write-Host "== $msg" -ForegroundColor Cyan }
function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red }

Write-Host '3DO to PC' -ForegroundColor White

if (-not $Inputs -or $Inputs.Count -eq 0) {
    Write-Host 'Drag a disc image (.iso, .bin, .cue) or a game folder into this window, then press Enter:'
    $typed = (Read-Host).Trim().Trim('"')
    if (-not $typed) { Done 1 }
    $Inputs = @($typed)
}

# ---------------------------------------------------------------- compiler
Step 'Looking for the C++ compiler'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = $null
if (Test-Path $vswhere) {
    # Prefer an install that has both the C++ compiler and the CMake tools.
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath | Select-Object -First 1
    if (-not $vs) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
    }
}
if (-not $vs) {
    Fail 'Visual Studio or Build Tools with C++ was not found.'
    Write-Host 'Install "Build Tools for Visual Studio 2022" (free) and tick "Desktop development with C++":'
    Write-Host '  https://visualstudio.microsoft.com/downloads/  (under Tools for Visual Studio)'
    Done 1
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
Write-Host "Using $vs"
# Import the 64-bit developer environment into this PowerShell session.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
# Find cmake.exe / ninja.exe in any Visual Studio install or a standalone CMake.
$rel = 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmakeExe = & $vswhere -all -products * -find "$rel\CMake\bin\cmake.exe" | Select-Object -First 1
if (-not $cmakeExe -and (Test-Path "$env:ProgramFiles\CMake\bin\cmake.exe")) { $cmakeExe = "$env:ProgramFiles\CMake\bin\cmake.exe" }
$ninjaExe = & $vswhere -all -products * -find "$rel\Ninja\ninja.exe" | Select-Object -First 1
foreach ($exe in $cmakeExe, $ninjaExe) {
    if ($exe) { $env:Path = "$(Split-Path -Parent $exe);$env:Path" }
}
if ($cmakeExe) { Write-Host "Using CMake: $cmakeExe" }
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Fail 'The C++ compiler (cl) was not found. Run Install-Prerequisites.bat first.'
    Done 1
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail 'CMake was not found in any Visual Studio install, in Program Files\CMake, or on PATH.'
    Write-Host 'Visual Studio installs:'; & $vswhere -all -products * -property installationPath | ForEach-Object { "  $_" }
    Write-Host 'Installs reporting the CMake component:'
    & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath | ForEach-Object { "  $_" }
    $ans = Read-Host 'Run Install-Prerequisites.bat now to add it? (Y/N)'
    if ($ans -match '^[Yy]') { Start-Process (Join-Path $Root 'Install-Prerequisites.bat') }
    Write-Host 'When it says "All set", drop the game on Convert-3DO.bat again.'
    Done 1
}
# Ninja is preferred; NMake comes with the C++ tools and works too.
$gen = if (Get-Command ninja -ErrorAction SilentlyContinue) { 'Ninja' } else { 'NMake Makefiles' }
Write-Host "Using generator: $gen"

# ---------------------------------------------------------------- converter
Step 'Building the converter (first time takes a minute)'
$build = Join-Path $Root 'build'
$cache = Join-Path $build 'CMakeCache.txt'
if ((Test-Path $cache) -and -not (Select-String -LiteralPath $cache -SimpleMatch "CMAKE_GENERATOR:INTERNAL=$gen" -Quiet)) {
    Remove-Item -Recurse -Force $build    # earlier build used another generator
}
if (-not (Test-Path $cache)) {
    cmake -S $Root -B $build -G $gen -DCMAKE_BUILD_TYPE=Release -DTDO_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed (see above).'; Done 1 }
}
cmake --build $build --target 3DOtoPC_cli
if ($LASTEXITCODE -ne 0) { Fail 'Building the converter failed (see above). Send me this window''s text.'; Done 1 }
$converter = Join-Path $build '3DO to PC CLI.exe'

# ---------------------------------------------------------------- games
$games = Join-Path $Root 'games'
New-Item -ItemType Directory -Force -Path $games | Out-Null
$exit = 0

# A game's other discs: "...(Disc 1 of 2)..." -> the same name with Disc 2, 3, ...
# beside it (dropped together or found in the same folder). Disc 1 is converted.
function Get-DiscNumber([string]$n) { if ($n -match '\(Disc\s*(\d+)') { [int]$Matches[1] } else { 1 } }
function Find-OtherDiscs([System.IO.FileSystemInfo]$item) {
    if ($item.Name -notmatch '\(Disc\s*(\d+)\s*of\s*(\d+)\)') { return @() }
    $mine = [int]$Matches[1]; $count = [int]$Matches[2]
    $parent = if ($item.PSIsContainer) { $item.Parent.FullName } else { $item.DirectoryName }
    $found = @()
    for ($k = 1; $k -le $count; $k++) {
        if ($k -eq $mine) { continue }
        $want = $item.Name -replace '\(Disc\s*\d+\s*of', "(Disc $k of"
        $p = Join-Path $parent $want
        if (Test-Path -LiteralPath $p) { $found += Get-Item -LiteralPath $p }
    }
    return $found
}
$handled = @{}
$Inputs = @($Inputs | Sort-Object { Get-DiscNumber (Split-Path -Leaf $_.Trim('"')) })
foreach ($in in $Inputs) {
    $path = $in.Trim('"')
    if (-not (Test-Path -LiteralPath $path)) { Fail "Not found: $path"; $exit = 1; continue }
    $item = Get-Item -LiteralPath $path
    if ($handled.ContainsKey($item.FullName)) { continue }    # converted with its disc 1
    $otherDiscs = @(Find-OtherDiscs $item | Where-Object { (Get-DiscNumber $_.Name) -gt (Get-DiscNumber $item.Name) })
    foreach ($d in $otherDiscs) { $handled[$d.FullName] = $true }
    $base = if ($item.PSIsContainer) { $item.Name } else { $item.BaseName }
    # A short name: disc-image names carry tags such as "(1993)(Electronic Arts)(US)[!]"
    # and subtitles, and a long name pushes Visual Studio's object-file paths
    # past its 250-character limit (it appears twice in each path).
    $short = $base -replace '\s*[\(\[][^\)\]]*[\)\]]', ''
    if ($short -match '^(.+?)\s+-\s+.+$') { $short = $Matches[1] }
    $name = (($short -replace '[^A-Za-z0-9_-]', '_') -replace '_+', '_').Trim('_')
    if ($name.Length -gt 40) { $name = $name.Substring(0, 40).TrimEnd('_') }
    if (-not $name) { $name = 'Game' }
    $out = Join-Path $games $name
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    Step "Converting $($item.Name) -> games\$name"
    $convArgs = @('convert', $item.FullName, $out, '--name', $name, '--runtime', (Join-Path $Root 'runtime'))
    foreach ($d in $otherDiscs) { $convArgs += @('--disc', $d.FullName); Write-Host "  with $($d.Name)" }
    $hints = Join-Path $out 'interp_hits.txt'
    if (Test-Path $hints) {
        Write-Host 'Reusing interp_hits.txt from the last run (those blocks get recompiled).'
        $convArgs += @('--hints', $hints)
    }
    # Ninja prints "[ 23% 23/100] Building CXX object ..." for every file (NINJA_STATUS);
    # turn that into a progress bar and a short line per file. Full output goes to convert.log.
    $env:NINJA_STATUS = '[%p %f/%t] '
    $logPath = Join-Path $out 'convert.log'
    Remove-Item -LiteralPath $logPath -ErrorAction SilentlyContinue
    & $converter @convArgs 2>&1 | ForEach-Object {
        $line = "$_"
        Add-Content -LiteralPath $logPath -Value $line
        if ($line -match '^\[\s*(\d+)%\s+(\d+)/(\d+)\]\s*(.*)$') {
            $pct = [int]$matches[1]; $done = $matches[2]; $total = $matches[3]
            $file = ($matches[4] -split '[\\/ ]')[-1]
            Write-Progress -Activity "Compiling $name" -Status "$done of $total files ($pct%)   $file" -PercentComplete $pct
            Write-Host ("  [{0,3}%] {1,4}/{2}  {3}" -f $pct, $done, $total, $file)
        } else {
            Write-Host $line
        }
    }
    $rc = $LASTEXITCODE
    Write-Progress -Activity "Compiling $name" -Completed
    if ($rc -ne 0) {
        Fail "Conversion failed. Send me games\$name\convert.log"
        $exit = 1
        continue
    }

    $exe = Join-Path $out "$name.exe"
    if (-not (Test-Path $exe)) { Fail "Expected $exe but it was not built."; $exit = 1; continue }

    Step "Running $name (close the game window when you are done)"
    Write-Host 'Keys: arrows = d-pad, Z/X/C = A/B/C, A/S = L/R, Enter = Play, Backspace = Stop. Xbox pads work too.'
    $log = Join-Path $out 'tdort.log'
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    Start-Process -FilePath $exe -WorkingDirectory $out -Wait

    Step 'Result'
    if (Test-Path $log) {
        $lines = Get-Content -LiteralPath $log
        $missing = @($lines | Where-Object { $_ -match 'unimplemented|stub|not supported|not found' })
        Write-Host "Log has $($lines.Count) lines, $($missing.Count) about missing OS features. Last lines:"
        $lines | Select-Object -Last 25 | ForEach-Object { Write-Host "  $_" }
        Write-Host ''
        Write-Host "Send me: $log" -ForegroundColor Yellow
        Start-Process explorer.exe -ArgumentList "/select,`"$log`""
    } else {
        Fail 'The game wrote no tdort.log (it may have crashed on start-up).'
        $exit = 1
    }
}

Done $exit
