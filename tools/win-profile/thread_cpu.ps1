# Per-thread CPU of the running game on Windows, in consecutive windows: thread name, share of one core, CPU ms per
# guest start (at 59.94 starts per second), and the load of each GPU engine the process uses. It only reads counters
# (GetThreadTimes, the "GPU Engine" performance counters); it does not attach to or pause the game.
#
#   pwsh tools\win-profile\thread_cpu.ps1 [-Windows 5] [-Seconds 14] [-SessionDir DIR]
#
# -SessionDir is the session folder of the run (its stderr.log names the scene phase each window covers); without it
# the newest folder under %LOCALAPPDATA%\Fukami\sessions is used. Thread names come from SetThreadDescription
# (GameThread, rrv-gs-owner, rrv-vu1, rrv-spu2); threads of SDL, PCSX2 and the graphics driver mostly have none.
# Thread times advance in scheduler ticks (15.6 ms), so read windows of several seconds, not single frames.
param([int]$Windows = 5, [int]$Seconds = 14, [string]$SessionDir = '')

Add-Type -Namespace RrvProfile -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
[DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr text);
[DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
[DllImport("kernel32.dll")] public static extern bool GetThreadTimes(IntPtr h, out long c, out long e, out long k, out long u);
'@

# The game is the Fukami.exe that was given the ELF; the other one is the launcher waiting for it.
$game = Get-CimInstance Win32_Process -Filter "Name='Fukami.exe'" | Where-Object { $_.CommandLine -match 'SLUS_200' }
if (-not $game) { 'no game process (Fukami.exe with the ELF argument)'; exit 1 }
$gamePid = [int]$game.ProcessId
if (-not $SessionDir) {
    $SessionDir = (Get-ChildItem "$env:LOCALAPPDATA\Fukami\sessions" | Sort-Object Name | Select-Object -Last 1).FullName
}

function Snapshot {
    $out = @{}
    foreach ($thread in (Get-Process -Id $gamePid).Threads) {
        $handle = [RrvProfile.Native]::OpenThread(0x0800, $false, [uint32]$thread.Id)  # THREAD_QUERY_LIMITED_INFORMATION
        if ($handle -eq [IntPtr]::Zero) { continue }
        $created = 0L; $exited = 0L; $kernel = 0L; $user = 0L
        [void][RrvProfile.Native]::GetThreadTimes($handle, [ref]$created, [ref]$exited, [ref]$kernel, [ref]$user)
        $text = [IntPtr]::Zero; $name = ''
        if ([RrvProfile.Native]::GetThreadDescription($handle, [ref]$text) -ge 0 -and $text -ne [IntPtr]::Zero) {
            $name = [Runtime.InteropServices.Marshal]::PtrToStringUni($text)
            [void][RrvProfile.Native]::LocalFree($text)
        }
        [void][RrvProfile.Native]::CloseHandle($handle)
        $out[$thread.Id] = [pscustomobject]@{ Name = $name; Kernel = $kernel; User = $user }
    }
    $out
}

function Scene {
    $line = Select-String -Path "$SessionDir\stderr.log" -Pattern 'gate3-observe\] start=(\d+) .*phase=(\d+)' -ErrorAction SilentlyContinue |
        Select-Object -Last 1
    if ($line) { "phase $($line.Matches[0].Groups[2].Value) (from start $($line.Matches[0].Groups[1].Value))" } else { 'boot' }
}

"pid $gamePid, $([Environment]::ProcessorCount) logical processors, session $SessionDir"
for ($window = 1; $window -le $Windows; $window++) {
    $before = Snapshot; $sceneBefore = Scene; $t0 = Get-Date
    # The counter read is also the wait: one sample over the whole window.
    $gpu = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval $Seconds -MaxSamples 1 -ErrorAction SilentlyContinue).CounterSamples |
        Where-Object { $_.InstanceName -match "pid_${gamePid}_" -and $_.CookedValue -gt 0.3 }
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { 'the game ended'; break }
    $after = Snapshot; $sceneAfter = Scene; $elapsed = ((Get-Date) - $t0).TotalSeconds
    "--- window ${window}: $([Math]::Round($elapsed, 1)) s, $sceneBefore to $sceneAfter"
    $rows = foreach ($id in $after.Keys) {
        if (-not $before.ContainsKey($id)) { continue }
        $user = ($after[$id].User - $before[$id].User) / 1e7
        $kernel = ($after[$id].Kernel - $before[$id].Kernel) / 1e7
        [pscustomobject]@{
            Tid = $id; Name = $after[$id].Name
            CorePct = [Math]::Round(100 * ($user + $kernel) / $elapsed, 1)
            UserPct = [Math]::Round(100 * $user / $elapsed, 1)
            KernelPct = [Math]::Round(100 * $kernel / $elapsed, 1)
            MsPerStart = [Math]::Round(1000 * ($user + $kernel) / ($elapsed * 59.94), 2)
        }
    }
    $rows | Where-Object { $_.CorePct -ge 0.5 } | Sort-Object CorePct -Descending | Format-Table -AutoSize | Out-String -Width 160
    "process: $([Math]::Round((($rows | Measure-Object CorePct -Sum).Sum), 1))% of one core over $($rows.Count) threads"
    $gpu | Group-Object { if ($_.InstanceName -match 'luid_(0x[0-9a-f]+_0x[0-9a-f]+).*engtype_(\w+)') { "$($Matches[1]) $($Matches[2])" } } |
        ForEach-Object { "gpu $($_.Name): $([Math]::Round(($_.Group | Measure-Object CookedValue -Sum).Sum, 1))%" }
}
