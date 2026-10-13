# One timed run of the Windows app on TEST data (never the owner's settings, saves or sessions), optionally with the
# per-thread CPU sampler, then the pacing summary. The Windows counterpart of tools/deck-profile/deck_run.sh.
#
#   pwsh tools\win-profile\run.ps1 -Tag d3d11-a [-App DIR] [-Seconds 95] [-Settings backend=d3d11,fullscreen=true]
#        [-EnvLines RRV_NAME=value,...] [-Sample] [-Chd FILE]
#
# -App       the app folder (default: the newest local\app\*-windows\Fukami, from scripts\package_fukami_windows.py)
# -Settings  rrv.ini keys for this run, applied to a fresh copy of the app's share\fukami-default.ini
# -EnvLines  [env] lines (developer mode is switched on for them: diagnostics, counters, captures)
# -Sample    run tools\win-profile\thread_cpu.ps1 from second 22 to the end of the run
# -Chd       the owner's CHD, only needed when neither the test data nor %LOCALAPPDATA%\Fukami has the unpacked disc
#
# Test data: local\win-test\data (FUKAMI_DATA_DIR): its own fukami.ini, mc\ and sessions\; disc\ is a junction to the
# owner's unpacked disc when there is one. The session folder is copied to local\win-profile\<Tag>. The run closes the
# game window as a user would; one game at a time (it refuses while another Fukami.exe runs). It only touches what it
# started: the game is the child of the launcher process this script launched (ParentProcessId), and the session is
# the folder of that launcher (the app names it <timestamp>-<launcher process id>, with -N added on a restart).
param([Parameter(Mandatory = $true)][string]$Tag, [string]$App = '', [int]$Seconds = 95, [string[]]$Settings = @(),
      [string[]]$EnvLines = @(), [switch]$Sample, [string]$Chd = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $App) {
    $App = (Get-ChildItem "$root\local\app" -Directory -Filter '*-windows' | Sort-Object LastWriteTime | Select-Object -Last 1).FullName + '\Fukami'
}
if (-not (Test-Path "$App\Fukami.exe")) { throw "no app at $App (python scripts\package_fukami_windows.py --build BUILD)" }
if (Get-Process Fukami -ErrorAction SilentlyContinue) { throw 'a Fukami.exe is already running: close it first (timing runs are single-instance)' }

$data = "$root\local\win-test\data"
New-Item -ItemType Directory -Force "$data\mc" | Out-Null
$ownerDisc = "$env:LOCALAPPDATA\Fukami\disc"
if (-not (Test-Path "$data\disc\.fukami-disc-verified") -and (Test-Path "$ownerDisc\.fukami-disc-verified")) {
    if (Test-Path "$data\disc") { Remove-Item "$data\disc" -Recurse -Force }
    New-Item -ItemType Junction -Path "$data\disc" -Target $ownerDisc | Out-Null
}
if (-not (Test-Path "$data\disc\.fukami-disc-verified") -and -not $Chd) { throw 'no unpacked disc: pass -Chd FILE once' }

# A fresh settings file per run: the app defaults plus this run's keys. Logging is on: the session folder and its
# start clocks are what this script reads (the app default is off).
$ini = [IO.File]::ReadAllText("$App\share\fukami-default.ini")
foreach ($setting in @('logging=true') + $Settings) {
    $key, $value = $setting -split '=', 2
    $pattern = "(?m)^$([regex]::Escape($key)) = .*$"
    if ($ini -notmatch $pattern) { throw "the default ini has no key '$key'" }
    $ini = [regex]::Replace($ini, $pattern, "$key = $value")
}
if ($EnvLines.Count) { $ini += "`n[developer]`nenabled = true`n`n[env]`n" + (($EnvLines | ForEach-Object { $_ -replace '=', ' = ' }) -join "`n") + "`n" }
[IO.File]::WriteAllText("$data\fukami.ini", $ini, (New-Object Text.UTF8Encoding $false))

$env:FUKAMI_DATA_DIR = $data
if ($Chd) { $env:FUKAMI_CHD = $Chd }
$started = Get-Date
$launcher = Start-Process "$App\Fukami.exe" -PassThru
$launcherId = $launcher.Id
# The newest session folder of THIS launcher (fukami_app_win.cpp: <timestamp>-<launcher pid>[-N]), or $null.
function Find-OwnSession {
    $own = Get-ChildItem "$data\sessions" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "-$launcherId(-\d+)?$" } | Sort-Object Name | Select-Object -Last 1
    if ($own) { $own.FullName }
}
try {
    $lead = [Math]::Min(22, [Math]::Max(5, $Seconds - 15))
    Start-Sleep $lead
    # Only the game this launcher started: its child Fukami.exe running the disc's SLUS_200 executable.
    $game = Get-CimInstance Win32_Process -Filter "Name='Fukami.exe' AND ParentProcessId=$launcherId" |
        Where-Object { $_.CommandLine -match 'SLUS_200' } | Select-Object -First 1
    if (-not $game) { throw "this run's game (child of launcher $launcherId) not found after $lead s (launcher exited: $($launcher.HasExited)); another run's game is left alone; see $data\sessions" }
    $session = Find-OwnSession
    if (-not $session) { throw "no session folder for launcher $launcherId in $data\sessions (is logging on?)" }
    $left = $Seconds - $lead
    if ($Sample) {
        $window = 14
        & "$PSScriptRoot\thread_cpu.ps1" -Windows ([Math]::Max(1, [Math]::Floor($left / ($window + 0.3)))) -Seconds $window -SessionDir $session
    } else {
        Start-Sleep $left
    }
    $process = Get-Process -Id $game.ProcessId -ErrorAction SilentlyContinue
    if ($process) { [void]$process.CloseMainWindow() }
    if (-not $launcher.WaitForExit(20000)) { 'the game did not close in 20 s; ending it'; $launcher.Kill() }
    "run: $([Math]::Round(((Get-Date) - $started).TotalSeconds)) s, launcher exit code $($launcher.ExitCode)"
} finally {
    Remove-Item Env:FUKAMI_DATA_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:FUKAMI_CHD -ErrorAction SilentlyContinue
    if (-not $launcher.HasExited) { $launcher.Kill() }
}
$session = Find-OwnSession   # a restart adds a -N folder for the same launcher: take the last one
if (-not $session) { throw "no session folder for launcher $launcherId in $data\sessions" }
$out = "$root\local\win-profile\$Tag"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Copy-Item $session $out -Recurse
"session copied to $out"
$python = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }
& $python "$PSScriptRoot\session_stats.py" $out
