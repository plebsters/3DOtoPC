# batch-test.ps1 - converts and test-runs a whole folder of 3DO games, unattended
# (called by Batch-Test.bat). Everything runs on this PC.
#
#   1. finds a folder's disc images (.iso / .cue / .bin) and game folders
#   2. converts each one into games\<Name>\ (same as Convert-3DO.bat)
#   3. runs it without a window for ~90 s of game time: first untouched, then
#      pressing Start / A / C / directions to get through menus; takes screenshots
#   4. writes batch-results\: one folder per game (log + screenshots),
#      summary.csv, report.html, and a zip of it all to send back
#
# Games already tested are skipped next time (use -Rerun to test them again),
# so it can be stopped with Ctrl+C and started again later.
#
# Options:  -Frames 5400       game frames per test (60 per second of game time)
#           -TimeoutMinutes 15 stop a test that takes longer than this
#           -ConvertTimeoutMinutes 60  stop a conversion that takes longer than this
#           -Rerun             test games again even if they have results
#           -Only "Gex*"       only games whose name matches this pattern
#           -NoPause           do not wait for Enter at the end

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Inputs,
    [int]$Frames = 5400,
    [int]$TimeoutMinutes = 15,
    [int]$ConvertTimeoutMinutes = 60,
    [switch]$Rerun,
    [string]$Only = '*',
    [switch]$NoPause
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot

function Done([int]$code) {
    if (-not $NoPause) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}
function Step([string]$msg) { Write-Host ''; Write-Host "== $msg" -ForegroundColor Cyan }
function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red }

Write-Host '3DO to PC - batch test' -ForegroundColor White

if (-not $Inputs -or $Inputs.Count -eq 0) {
    Write-Host 'Drag the folder that holds your 3DO disc images or game folders into this window, then press Enter:'
    $typed = (Read-Host).Trim().Trim('"')
    if (-not $typed) { Done 1 }
    $Inputs = @($typed)
}

# ---------------------------------------------------------------- games to test
# A game is a disc image (.iso, .cue, or a .bin with no .cue beside it) or a
# folder holding a LaunchMe (the 3DO's boot program), at the top or in data\.
function Test-GameFolder([string]$dir) {
    foreach ($p in @($dir, (Join-Path $dir 'data'))) {
        if (Test-Path -LiteralPath (Join-Path $p 'LaunchMe')) { return $true }
    }
    return $false
}

$candidates = New-Object System.Collections.Generic.List[System.IO.FileSystemInfo]
foreach ($in in $Inputs) {
    $path = $in.Trim('"')
    if (-not (Test-Path -LiteralPath $path)) { Fail "Not found: $path"; continue }
    $item = Get-Item -LiteralPath $path
    if (-not $item.PSIsContainer) { $candidates.Add($item); continue }
    if (Test-GameFolder $item.FullName) { $candidates.Add($item); continue }   # one game folder
    $files = @(Get-ChildItem -LiteralPath $item.FullName -File -Recurse -Depth 2 -ErrorAction SilentlyContinue |
               Where-Object { $_.Extension -in '.iso', '.cue', '.bin' })
    foreach ($f in $files) {
        if ($f.Extension -eq '.bin') {
            # a .bin listed by a .cue is converted through the .cue
            $cue = Join-Path $f.DirectoryName ($f.BaseName + '.cue')
            if (Test-Path -LiteralPath $cue) { continue }
            if (@(Get-ChildItem -LiteralPath $f.DirectoryName -Filter *.cue -ErrorAction SilentlyContinue).Count -gt 0) { continue }
        }
        $candidates.Add($f)
    }
    foreach ($d in @(Get-ChildItem -LiteralPath $item.FullName -Directory -ErrorAction SilentlyContinue)) {
        if (Test-GameFolder $d.FullName) { $candidates.Add($d) }
    }
}

# Same short names as convert.ps1, so the results line up with games\<Name>\.
function Get-ShortName([System.IO.FileSystemInfo]$item) {
    $base = if ($item.PSIsContainer) { $item.Name } else { $item.BaseName }
    $short = $base -replace '\s*[\(\[][^\)\]]*[\)\]]', ''
    if ($short -match '^(.+?)\s+-\s+.+$') { $short = $Matches[1] }
    $name = (($short -replace '[^A-Za-z0-9_-]', '_') -replace '_+', '_').Trim('_')
    if ($name.Length -gt 40) { $name = $name.Substring(0, 40).TrimEnd('_') }
    if (-not $name) { $name = 'Game' }
    return $name
}

# Discs of one game share its short name ("... (Disc 1 of 2)", "(Disc 2 of 2)"):
# disc 1 is converted, the others go along with --disc.
# Which copy of a disc to use when there are several: a .cue first (it knows the
# track layout), then an .iso, an extracted folder, a bare .bin.
function Get-SourceRank([System.IO.FileSystemInfo]$item) {
    if ($item.PSIsContainer) { return 2 }
    switch ($item.Extension.ToLower()) { '.cue' { 0 } '.iso' { 1 } default { 3 } }
}
function Get-DiscNumber([System.IO.FileSystemInfo]$item) {
    if ($item.Name -match '\(Disc\s*(\d+)') { return [int]$Matches[1] }
    return 1
}
$jobs = @()
foreach ($grp in ($candidates | Group-Object { Get-ShortName $_ })) {
    $n = $grp.Name
    if ($n -notlike $Only) { continue }
    # Only a different disc number makes another disc. Copies of the same disc
    # (an .iso and a .cue of one game, two releases, a folder from an earlier
    # extract) are one disc: keep the best copy and say which were skipped.
    $byDisc = @($grp.Group | Group-Object { Get-DiscNumber $_ } | Sort-Object { [int]$_.Name })
    $picked = @()
    foreach ($d in $byDisc) {
        $best = @($d.Group | Sort-Object { Get-SourceRank $_ }, Name)
        $picked += $best[0]
        foreach ($dup in ($best | Select-Object -Skip 1)) { Write-Host "  $n : skipping $($dup.Name) (another copy of disc $($d.Name))" -ForegroundColor DarkGray }
    }
    $jobs += [pscustomobject]@{ Name = $n; Item = $picked[0]; Extra = @($picked | Select-Object -Skip 1) }
}
$jobs = @($jobs | Sort-Object Name)
if ($jobs.Count -eq 0) {
    Fail 'No 3DO games found there (looked for .iso / .cue / .bin files and folders with a LaunchMe).'
    Done 1
}
Write-Host "Found $($jobs.Count) game(s)."

# ---------------------------------------------------------------- compiler (as in convert.ps1)
Step 'Looking for the C++ compiler'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath | Select-Object -First 1
    if (-not $vs) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
    }
}
if (-not $vs) { Fail 'Visual Studio or Build Tools with C++ was not found. Run Install-Prerequisites.bat first.'; Done 1 }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$rel = 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmakeExe = & $vswhere -all -products * -find "$rel\CMake\bin\cmake.exe" | Select-Object -First 1
if (-not $cmakeExe -and (Test-Path "$env:ProgramFiles\CMake\bin\cmake.exe")) { $cmakeExe = "$env:ProgramFiles\CMake\bin\cmake.exe" }
$ninjaExe = & $vswhere -all -products * -find "$rel\Ninja\ninja.exe" | Select-Object -First 1
foreach ($exe in $cmakeExe, $ninjaExe) { if ($exe) { $env:Path = "$(Split-Path -Parent $exe);$env:Path" } }
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { Fail 'The C++ compiler (cl) was not found. Run Install-Prerequisites.bat first.'; Done 1 }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail 'CMake was not found. Run Install-Prerequisites.bat first.'; Done 1 }
$gen = if (Get-Command ninja -ErrorAction SilentlyContinue) { 'Ninja' } else { 'NMake Makefiles' }

Step 'Building the converter'
$build = Join-Path $Root 'build'
$cache = Join-Path $build 'CMakeCache.txt'
if ((Test-Path $cache) -and -not (Select-String -LiteralPath $cache -SimpleMatch "CMAKE_GENERATOR:INTERNAL=$gen" -Quiet)) {
    Remove-Item -Recurse -Force $build
}
if (-not (Test-Path $cache)) {
    cmake -S $Root -B $build -G $gen -DCMAKE_BUILD_TYPE=Release -DTDO_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed (see above).'; Done 1 }
}
cmake --build $build --target 3DOtoPC_cli
if ($LASTEXITCODE -ne 0) { Fail 'Building the converter failed (see above).'; Done 1 }
$converter = Join-Path $build '3DO to PC CLI.exe'

# ---------------------------------------------------------------- test settings
# Screenshots at 5 points; the last two show whether the screen still changes.
$snapFrames = @(600, [int]($Frames * 0.35), [int]($Frames * 0.6), [int]($Frames * 0.85), ($Frames - 30)) |
              Where-Object { $_ -gt 0 -and $_ -lt $Frames } | Sort-Object -Unique
# Buttons: nothing for the first 30 s (logos, intros, attract mode), then a
# routine every 2.5 s that gets most games through title screens and menus.
$routine = @('P', 'A', 'A', 'C', 'D', 'A', 'P', 'A', 'r', 'A', 'B', 'A')
$press = @()
$i = 0
for ($f = 1800; $f -lt $Frames - 60; $f += 150) {
    $press += "$f-$($f + 6):$($routine[$i % $routine.Count])"
    $i++
}
$pressSpec = $press -join ','

$gamesDir = Join-Path $Root 'games'
$results = Join-Path $Root 'batch-results'
New-Item -ItemType Directory -Force -Path $gamesDir, $results | Out-Null
Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue

function Save-Png([string]$bmp, [string]$png) {
    try {
        $img = [System.Drawing.Image]::FromFile($bmp)
        try { $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $img.Dispose() }
        Remove-Item -LiteralPath $bmp -Force
        return $png
    } catch {
        return $bmp   # keep the .bmp if it cannot be converted
    }
}

function Get-FileHashOrEmpty([string]$p) {
    if ($p -and (Test-Path -LiteralPath $p)) { return (Get-FileHash -LiteralPath $p -Algorithm MD5).Hash }
    return ''
}

# Reads a game's tdort.log and sums up what happened.
function Get-LogFacts([string]$log) {
    $facts = [ordered]@{ Frames = 0; Cels = 0; Crash = ''; Exited = $false; Problems = @() }
    if (-not (Test-Path -LiteralPath $log)) { return $facts }
    $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
    if ($lines.Count -eq 0) { return $facts }
    foreach ($l in $lines) {
        if ($l -match '^frame (\d+):') {
            $facts.Frames = [int]$Matches[1]
            if ($l -match 'cels=(\d+)') { $facts.Cels = [int]$Matches[1] }
        }
    }
    $ci = [Array]::FindIndex([string[]]$lines, [Predicate[string]] { param($x) $x -match '== CRASH ==' })
    if ($ci -ge 0) {
        $reason = $lines[($ci + 1)..([Math]::Min($ci + 2, $lines.Count - 1))] -join ' '
        $facts.Crash = ($reason -replace '\s+', ' ').Trim()
    }
    # The game's main program ending means the game quit (the log ends the
    # same way whether it quit or the test's frame limit was reached).
    if (@($lines | Where-Object { $_ -match '^task "LaunchMe" ended|^guest exit\(-?\d+\) from "LaunchMe"|^exit\(\) from the main program' }).Count -gt 0) { $facts.Exited = $true }
    # Missing or unsupported OS features, without addresses/arguments, once each.
    $facts.Problems = @($lines | Where-Object {
            $_ -match 'unimplemented|HLE stub|not supported|bad device|CreateIOReq: bad|unknown (tag|device|command)|failed'
        } | ForEach-Object {
            # keep what is missing (SWI number, folio call), drop addresses, arguments and results
            ($_ -replace '\(.*\)', '(...)' -replace '\s*->.*$', '' -replace '\b[0-9A-F]{8}\b', '#' -replace '-?\b\d{5,}\b', '#').Trim()
        } |
        Sort-Object -Unique)
    return $facts
}

# ---------------------------------------------------------------- run
$n = 0
foreach ($job in $jobs) {
    $n++
    $name = $job.Name
    $resDir = Join-Path $results $name
    $resFile = Join-Path $resDir 'result.json'
    if ((Test-Path $resFile) -and -not $Rerun) { Write-Host "[$n/$($jobs.Count)] $name - already tested, skipping"; continue }
    New-Item -ItemType Directory -Force -Path $resDir | Out-Null
    Get-ChildItem -LiteralPath $resDir -File -ErrorAction SilentlyContinue | Remove-Item -Force

    $started = Get-Date
    $srcName = if ($job.Extra.Count) { "$($job.Item.Name) (+$($job.Extra.Count) more disc$(if ($job.Extra.Count -gt 1) {'s'}))" } else { $job.Item.Name }
    $res = [ordered]@{ Game = $name; Source = $srcName; Status = ''; Detail = ''; Frames = 0; Cels = 0;
                       ScreenChanges = ''; Minutes = 0; Diagnosis = ''; Problems = @() }

    Step "[$n/$($jobs.Count)] $name  ($($job.Item.Name))"
    $out = Join-Path $gamesDir $name
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $convArgs = @('convert', $job.Item.FullName, $out, '--name', $name, '--runtime', (Join-Path $Root 'runtime'))
    foreach ($d in $job.Extra) { $convArgs += @('--disc', $d.FullName) }
    if ($job.Extra.Count) { Write-Host "$($job.Extra.Count + 1) discs" }
    $hints = Join-Path $out 'interp_hits.txt'
    if (Test-Path $hints) { $convArgs += @('--hints', $hints) }
    $convLog = Join-Path $out 'convert.log'
    Write-Host "Converting (a few minutes; stopped after $ConvertTimeoutMinutes)..."
    # Run the converter as its own process so a conversion that hangs can be
    # stopped (with the compiler processes it started) and the batch goes on.
    $quoted = ($convArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $convErr = "$convLog.err"
    $cp = Start-Process -FilePath $converter -ArgumentList $quoted -PassThru -NoNewWindow `
                        -RedirectStandardOutput $convLog -RedirectStandardError $convErr
    $null = $cp.Handle
    $convDone = $cp.WaitForExit($ConvertTimeoutMinutes * 60 * 1000)
    if (-not $convDone) {
        & taskkill.exe /PID $cp.Id /T /F *> $null
        Start-Sleep -Seconds 2
    }
    if (Test-Path -LiteralPath $convErr) {
        Get-Content -LiteralPath $convErr -ErrorAction SilentlyContinue | Add-Content -LiteralPath $convLog
        Remove-Item -LiteralPath $convErr -ErrorAction SilentlyContinue
    }
    $convCode = if ($convDone) { $cp.ExitCode } else { -1 }
    $exe = Join-Path $out "$name.exe"
    if (-not $convDone) {
        $res.Status = 'Conversion timed out'
        $last = (Get-Content -LiteralPath $convLog -ErrorAction SilentlyContinue | Select-Object -Last 1) -as [string]
        $res.Detail = "Still converting after $ConvertTimeoutMinutes minutes; last step: $last"
        Copy-Item -LiteralPath $convLog -Destination $resDir -ErrorAction SilentlyContinue
    } elseif ($convCode -ne 0 -or -not (Test-Path $exe)) {
        $res.Status = 'Conversion failed'
        $res.Detail = ((Get-Content -LiteralPath $convLog -ErrorAction SilentlyContinue |
                        Where-Object { $_ -match 'error|fail' } | Select-Object -First 1) -as [string])
        Copy-Item -LiteralPath $convLog -Destination $resDir -ErrorAction SilentlyContinue
    } else {
        Write-Host "Running for $Frames frames without a window..."
        Get-ChildItem -LiteralPath $out -Filter 'snap_*.bmp' -ErrorAction SilentlyContinue | Remove-Item -Force
        $log = Join-Path $out 'tdort.log'
        Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $out 'nvram') -Recurse -Force -ErrorAction SilentlyContinue   # start without saves
        $env:TDORT_FRAMES = "$Frames"
        $env:TDORT_SNAP = ($snapFrames -join ',')
        $env:TDORT_PRESS = $pressSpec
        $env:TDORT_AUDIO_STATUS = '1'          # status lines for the whole run, not just the first minute
        $p = Start-Process -FilePath $exe -ArgumentList '--headless' -WorkingDirectory $out -PassThru -WindowStyle Hidden
        $null = $p.Handle                       # needed for ExitCode to be readable after it ends
        $finished = $p.WaitForExit($TimeoutMinutes * 60 * 1000)
        if (-not $finished) { try { $p.Kill() } catch {} ; Start-Sleep -Seconds 1 }
        Remove-Item Env:TDORT_FRAMES, Env:TDORT_SNAP, Env:TDORT_PRESS, Env:TDORT_AUDIO_STATUS -ErrorAction SilentlyContinue

        $facts = Get-LogFacts $log
        $res.Frames = $facts.Frames; $res.Cels = $facts.Cels; $res.Problems = $facts.Problems
        foreach ($s in $snapFrames) {
            $bmp = Join-Path $out "snap_$s.bmp"
            if (Test-Path -LiteralPath $bmp) { $null = Save-Png $bmp (Join-Path $resDir "snap_$s.png") }
        }
        # Any screenshot that could not be converted to PNG goes along as .bmp
        Get-ChildItem -LiteralPath $out -Filter 'snap_*.bmp' -ErrorAction SilentlyContinue | Move-Item -Destination $resDir -Force
        $lastTwo = @(Get-ChildItem -LiteralPath $resDir -Filter 'snap_*' | Sort-Object { [int]($_.BaseName -replace '\D', '') } | Select-Object -Last 2)
        if ($lastTwo.Count -eq 2) {
            $res.ScreenChanges = if ((Get-FileHashOrEmpty $lastTwo[0].FullName) -eq (Get-FileHashOrEmpty $lastTwo[1].FullName)) { 'no' } else { 'yes' }
        }

        if (-not (Test-Path $log))      { $res.Status = 'No log';  $res.Detail = 'The game wrote no tdort.log (crashed on start-up?)' }
        elseif (-not $finished)         { $res.Status = 'Timed out'; $res.Detail = "Still running after $TimeoutMinutes minutes" }
        elseif ($facts.Crash)           { $res.Status = 'Crashed'; $res.Detail = $facts.Crash }
        elseif ($facts.Exited -and $facts.Frames -lt $Frames - 240) { $res.Status = 'Exited'; $res.Detail = 'The game ended by itself' }
        elseif ($p.ExitCode -ne 0 -and $facts.Frames -lt $Frames - 240) { $res.Status = 'Crashed'; $res.Detail = "Program stopped early (exit code $($p.ExitCode))" }
        elseif (@(Get-ChildItem -LiteralPath $resDir -Filter 'snap_*' -ErrorAction SilentlyContinue).Count -eq 0) {
            $res.Status = 'Stopped early'; $res.Detail = 'The game stopped before the first screenshot'
        }
        elseif ($res.ScreenChanges -eq 'no') { $res.Status = 'Screen stuck'; $res.Detail = 'The last two screenshots are identical' }
        else                            { $res.Status = 'Running'; $res.Detail = 'Still running and drawing at the end of the test' }

        if (Test-Path $log) { Copy-Item -LiteralPath $log -Destination $resDir }
        # The runtime's own diagnosis (tdort_report.txt): its VERDICT line says
        # why the game stalled or crashed, with what each thread waits on.
        $report = Join-Path $out 'tdort_report.txt'
        if (Test-Path $report) {
            Copy-Item -LiteralPath $report -Destination $resDir
            $v = Select-String -LiteralPath $report -Pattern '^VERDICT: (.*)$' | Select-Object -First 1
            if ($v) { $res.Diagnosis = $v.Matches[0].Groups[1].Value }
        }
        if ($res.Status -ne 'Running') { Copy-Item -LiteralPath $convLog -Destination $resDir -ErrorAction SilentlyContinue }
    }
    $res.Minutes = [Math]::Round(((Get-Date) - $started).TotalMinutes, 1)
    $color = switch ($res.Status) { 'Running' { 'Green' } 'Screen stuck' { 'Yellow' } default { 'Red' } }
    Write-Host "$($res.Status): $($res.Detail)" -ForegroundColor $color
    if ($res.Diagnosis -and $res.Status -ne 'Running') { Write-Host "  Diagnosis: $($res.Diagnosis)" -ForegroundColor DarkGray }
    ($res | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $resFile -Encoding UTF8
}

# ---------------------------------------------------------------- report
Step 'Writing the report'
$all = @(Get-ChildItem -LiteralPath $results -Directory | ForEach-Object {
        $f = Join-Path $_.FullName 'result.json'
        if (Test-Path $f) { Get-Content -LiteralPath $f -Raw | ConvertFrom-Json }
    } | Sort-Object Game)

$all | Select-Object Game, Status, Detail, Diagnosis, Frames, Cels, ScreenChanges, Minutes, Source,
    @{ n = 'Problems'; e = { ($_.Problems -join ' | ') } } |
    Export-Csv -LiteralPath (Join-Path $results 'summary.csv') -NoTypeInformation -Encoding UTF8

# Missing OS features across all games, most common first.
$counts = @{}
foreach ($r in $all) { foreach ($p in @($r.Problems)) { if ($p) { $counts[$p] = 1 + [int]$counts[$p] } } }
$top = $counts.GetEnumerator() | Sort-Object Value -Descending
$lines = @($top | ForEach-Object { '{0,4} games  {1}' -f $_.Value, $_.Key })
# Then each game that didn't keep running, with the runtime's diagnosis.
$lines += ''
$lines += 'Diagnosis of each game that crashed, stalled or stopped:'
foreach ($r in ($all | Where-Object { $_.Status -ne 'Running' })) {
    $lines += ''
    $lines += "$($r.Game)  [$($r.Status)]"
    $lines += "  $(if ($r.Diagnosis) { $r.Diagnosis } else { $r.Detail })"
}
$lines | Set-Content -LiteralPath (Join-Path $results 'problems.txt') -Encoding UTF8

$enc = { param($s) [System.Net.WebUtility]::HtmlEncode([string]$s) }
$byStatus = $all | Group-Object Status | Sort-Object Count -Descending
$rows = foreach ($r in $all) {
    $imgs = (Get-ChildItem -LiteralPath (Join-Path $results $r.Game) -Filter 'snap_*' -ErrorAction SilentlyContinue |
             Sort-Object { [int]($_.BaseName -replace '\D', '') } |
             ForEach-Object { "<img src=`"$($r.Game)/$($_.Name)`" title=`"$($_.BaseName)`">" }) -join ''
    $cls = switch ($r.Status) { 'Running' { 'ok' } 'Screen stuck' { 'warn' } default { 'bad' } }
    "<tr><td><b>$(& $enc $r.Game)</b><br><small>$(& $enc $r.Source)</small></td><td class=$cls>$(& $enc $r.Status)</td><td>$(& $enc $r.Detail)$(if ($r.Diagnosis -and $r.Status -ne 'Running') { "<br><small class=diag>$(& $enc $r.Diagnosis)</small>" })<br><small>frame $($r.Frames), $($r.Cels) sprites, $($r.Minutes) min</small></td><td class=imgs>$imgs</td></tr>"
}
$html = @"
<!DOCTYPE html><html><head><meta charset="utf-8"><title>3DO to PC batch test</title>
<style>body{font-family:Segoe UI,sans-serif;background:#16161c;color:#eee;margin:24px}table{border-collapse:collapse;width:100%}
td{border-bottom:1px solid #333;padding:8px;vertical-align:top}.diag{color:#9ab}.ok{color:#6c6}.warn{color:#eb4}.bad{color:#f66}
.imgs img{width:160px;margin:0 4px 4px 0;image-rendering:pixelated}small{color:#999}h1{font-weight:600}</style></head><body>
<h1>3DO to PC batch test</h1><p>$(Get-Date -Format 'yyyy-MM-dd HH:mm') &middot; $($all.Count) games &middot;
$(($byStatus | ForEach-Object { "$($_.Count) $($_.Name)" }) -join ' &middot; ')</p>
<table><tr><th align=left>Game</th><th align=left>Result</th><th align=left>Details</th><th align=left>Screenshots</th></tr>
$($rows -join "`n")
</table></body></html>
"@
Set-Content -LiteralPath (Join-Path $results 'report.html') -Value $html -Encoding UTF8

$zip = Join-Path $Root ("batch-results-{0}.zip" -f (Get-Date -Format 'yyyy-MM-dd_HHmm'))
Compress-Archive -Path (Join-Path $results '*') -DestinationPath $zip -Force
Write-Host ''
$byStatus | ForEach-Object { Write-Host ("  {0,4}  {1}" -f $_.Count, $_.Name) }
Write-Host ''
Write-Host "Report: $(Join-Path $results 'report.html')"
Write-Host "Send me: $zip" -ForegroundColor Yellow
Start-Process (Join-Path $results 'report.html')
Start-Process explorer.exe -ArgumentList "/select,`"$zip`""
Done 0
