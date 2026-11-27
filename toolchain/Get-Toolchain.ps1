<#
Get-Toolchain.ps1 - puts the compiler that converts games into a toolchain\ folder.

    powershell -ExecutionPolicy Bypass -File toolchain\Get-Toolchain.ps1 [-Dest dist\toolchain]

It downloads three pinned packages from PyPI (the Python package index, which
publishes the official Windows builds as zip files), checks each one's SHA-256,
and unpacks:
    zig 0.17.1    -> <Dest>\zig     clang + MinGW-w64 headers/libraries (MIT licence)
    CMake 3.31.6  -> <Dest>\cmake   (BSD licence)
    Ninja 1.11.1  -> <Dest>\ninja   (Apache 2.0 licence)
then builds the zig-cc/zig-c++/zig-ar/zig-ranlib/zig-rc front ends from
zigwrap.cpp and copies zig-windows.cmake. Library sources zig has for other
systems (Linux, BSD, macOS, WebAssembly) are left out.

Build-App.bat runs this (dist\toolchain), so the installer contains the
compiler and the people who install 3DO to PC need nothing else: no Visual
Studio, no Windows SDK, no Visual C++ Redistributable. Downloads are kept in
build\downloads and reused; a toolchain that is already complete is left alone.
#>
param(
    [string]$Dest = (Join-Path $PSScriptRoot "..\dist\toolchain"),
    [string]$Downloads = (Join-Path $PSScriptRoot "..\build\downloads")
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is much faster without the progress bar
Add-Type -AssemblyName System.IO.Compression.FileSystem

$Stamp = "zig-0.17.1 cmake-3.31.6 ninja-1.11.1.4 wrap-3"
$Packages = @(
    @{ Name = 'zig';   File = 'ziglang-0.17.1-py3-none-win_amd64.whl'
       Url  = 'https://files.pythonhosted.org/packages/c8/6c/2013114fa780f0cf490cdc98bdd5bf7dce706a5dbd86e50ec4132cf7d4b1/ziglang-0.17.1-py3-none-win_amd64.whl'
       Sha  = '69c3dfc3aff6a297ec4017aa904782c0e450eb988982d7f675d83edce27526fb' },
    @{ Name = 'cmake'; File = 'cmake-3.31.6-py3-none-win_amd64.whl'
       Url  = 'https://files.pythonhosted.org/packages/18/58/909d6d99acb4e0886d0f660cf4e0fb26f586590e370b2e4ce7a10d06b145/cmake-3.31.6-py3-none-win_amd64.whl'
       Sha  = 'bbaed969cef3c427f4f17591feb28db4ae595e3a4bbd45cb35522cee14df6a32' },
    @{ Name = 'ninja'; File = 'ninja-1.11.1.4-py3-none-win_amd64.whl'
       Url  = 'https://files.pythonhosted.org/packages/5b/10/9b8fe9ac004847490cc7b54896124c01ce2d87d95dc60aabd0b8591addff/ninja-1.11.1.4-py3-none-win_amd64.whl'
       Sha  = '4617b3c12ff64b611a7d93fd9e378275512bb36eff8babff7c83f5116b4f8d66' }
)

$Dest = [IO.Path]::GetFullPath($Dest)
$Downloads = [IO.Path]::GetFullPath($Downloads)
$stampFile = Join-Path $Dest "toolchain-version.txt"
if ((Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -eq $Stamp) -and
    (Test-Path (Join-Path $Dest "zig\zig-c++.exe"))) {
    Write-Host "Toolchain already in place: $Dest"
    exit 0
}

New-Item -ItemType Directory -Force $Downloads | Out-Null
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force $Dest | Out-Null

function Get-Package($p) {
    $file = Join-Path $Downloads $p.File
    if (Test-Path $file) {
        if ((Get-FileHash $file -Algorithm SHA256).Hash -ieq $p.Sha) { return $file }
        Remove-Item -Force $file
    }
    Write-Host "Downloading $($p.Name) ($($p.File))..."
    Invoke-WebRequest -Uri $p.Url -OutFile $file -UseBasicParsing
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash
    if ($hash -ine $p.Sha) {
        Remove-Item -Force $file
        throw "$($p.File): checksum mismatch (got $hash, expected $($p.Sha))"
    }
    return $file
}

# Unpacks the entries of a wheel (zip) under $prefix into $target, keeping the rest of their paths.
function Expand-Prefix($zipFile, $prefix, $target, [scriptblock]$skip = { $false }) {
    $zip = [IO.Compression.ZipFile]::OpenRead($zipFile)
    try {
        foreach ($e in $zip.Entries) {
            if (-not $e.FullName.StartsWith($prefix) -or $e.FullName.EndsWith('/')) { continue }
            $rel = $e.FullName.Substring($prefix.Length)
            if (& $skip $rel) { continue }
            $out = Join-Path $target ($rel -replace '/', '\')
            New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
        }
    } finally { $zip.Dispose() }
}

$zigWhl, $cmakeWhl, $ninjaWhl = $Packages | ForEach-Object { Get-Package $_ }

Write-Host "Unpacking zig..."
# Keep the Windows parts of zig's C library; the sources for other systems are not used.
$otherLibc = '^lib/libc/(musl|wasi|glibc|darwin|netbsd|freebsd|openbsd)/|^lib/libtsan/|^lib/docs/|^doc/'
Expand-Prefix $zigWhl 'ziglang/' (Join-Path $Dest 'zig') {
    param($rel)
    if ($rel -match $otherLibc) { return $true }
    if ($rel -match '^lib/libc/include/([^/]+)/') { return -not ($Matches[1] -match 'windows|mingw') }
    if ($rel -match '\.py$') { return $true }
    return $false
}
Write-Host "Unpacking CMake..."
Expand-Prefix $cmakeWhl 'cmake/data/' (Join-Path $Dest 'cmake') { param($rel) $rel -match '^(doc|man)/|^bin/cmake-gui\.exe$' }
Write-Host "Unpacking Ninja..."
Expand-Prefix $ninjaWhl 'ninja-1.11.1.4.data/scripts/' (Join-Path $Dest 'ninja')

# Licences of what is shipped.
$lic = Join-Path $Dest 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $Dest 'zig\LICENSE') (Join-Path $lic 'zig-LICENSE.txt') -ErrorAction SilentlyContinue
Expand-Prefix $cmakeWhl 'cmake-3.31.6.dist-info/licenses/' (Join-Path $lic 'cmake')
Expand-Prefix $ninjaWhl 'ninja-1.11.1.4.dist-info/licenses/' (Join-Path $lic 'ninja')

foreach ($f in 'zig\zig.exe', 'cmake\bin\cmake.exe', 'ninja\ninja.exe') {
    if (-not (Test-Path (Join-Path $Dest $f))) { throw "missing after unpacking: $f" }
}

Write-Host "Building the zig front ends (the first zig run prepares its cache, about a minute)..."
$zig = Join-Path $Dest 'zig\zig.exe'
$wrap = Join-Path $Dest 'zig\zig-c++.exe'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Downloads 'zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $Downloads 'zig-cache'
# One copy per tool, with the tool built in: Ninja may start them by their 8.3
# short names (ZIG-C_~1.EXE), which don't say which tool they are.
$tools = 'c++', 'cc', 'ar', 'ranlib', 'rc'
for ($i = 0; $i -lt $tools.Count; $i++) {
    $t = $tools[$i]
    $exe = Join-Path $Dest "zig\zig-$t.exe"
    & $zig c++ -O2 -s "-DZIGWRAP_TOOL=$($i + 1)" -o $exe (Join-Path $PSScriptRoot 'zigwrap.cpp')
    if ($LASTEXITCODE -or -not (Test-Path $exe)) { throw "building zigwrap.cpp ($t) failed" }
    Remove-Item ([IO.Path]::ChangeExtension($exe, '.pdb')) -ErrorAction SilentlyContinue
}
Copy-Item (Join-Path $PSScriptRoot 'zig-windows.cmake') $Dest

# Check that the bundle compiles a C++ program through the front ends.
$test = Join-Path $Downloads 'tc-test'
New-Item -ItemType Directory -Force $test | Out-Null
Set-Content -Path (Join-Path $test 't.cpp') -Value '#include <string>
#include <cstdio>
int main() { std::string s = "ok"; std::printf("%s\n", s.c_str()); return 0; }'
& (Join-Path $Dest 'zig\zig-c++.exe') -O1 -o (Join-Path $test 't.exe') (Join-Path $test 't.cpp')
if ($LASTEXITCODE) { throw "the bundled compiler could not build a test program" }
$out = & (Join-Path $test 't.exe')
if ($out -ne 'ok') { throw "the test program built by the bundled compiler printed '$out'" }

Set-Content -Path $stampFile -Value $Stamp
$mb = [math]::Round(((Get-ChildItem -Recurse $Dest | Measure-Object -Property Length -Sum).Sum) / 1MB)
Write-Host "Toolchain ready in $Dest ($mb MB)."
