# Stage 1 acceptance runbook (spec section 9, plan task 11).
#
# *** OWNER-RUN ONLY ***
# This script installs the hook DLL into the LIVE Explorer session
# (`TrayMirror --duration N` installs/unloads per cycle). Per the repo
# safety rules the agent never runs it; the project owner runs it and
# shares the produced log for analysis.
#
# Emergency recovery, if the shell misbehaves:
#   taskkill /f /im explorer.exe; start explorer.exe
# The host's own kill switch: run `TrayMirror.exe --disable-hook` (arms
# Local\TrayMirror.Off; a resident hook goes inert forever).
#
# Criteria checked here (spec section 9, stage 1 acceptance):
#   1. selftest is green BEFORE any live contact (spec 12.1);
#   2. 100x host start/stop: every cycle exits 0, no Explorer restart,
#      the hook DLL leaves explorer.exe's module list after every stop;
#   3. no runaway growth in Explorer handle count / working set;
#   4. emulator phase: every NIM_* operation (x64 and WOW64 emulators)
#      is visible, decoded, in the host log.
#
# Usage:
#   powershell -File scripts\stage1-accept.ps1                 # full run (~10 min)
#   powershell -File scripts\stage1-accept.ps1 -SkipEmulator   # cycles only
#   powershell -File scripts\stage1-accept.ps1 -Cycles 5       # smoke subset

param(
    [int]$Cycles = 100,
    [int]$DurationSec = 5,
    [int]$EmuHostSec = 120,
    [switch]$SkipEmulator
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$HostExe = Join-Path $Root 'build\src\host\TrayMirror.exe'
$Emu64 = Join-Path $Root 'build\spikes\s1\emu\TrayIconEmu.exe'
$Emu86 = Join-Path $Root 'build-x86\spikes\s1\emu\TrayIconEmu.exe'
$LogPath = Join-Path $Root 'build\stage1-accept.log'
$HostLog = Join-Path $env:LOCALAPPDATA 'TrayMirror\logs\host.log'

function Write-Log([string]$Line) {
    $stamp = Get-Date -Format 'HH:mm:ss.fff'
    "$stamp $Line" | Tee-Object -FilePath $LogPath -Append
}

# FindWindow + GetWindowThreadProcessId: the Explorer process that owns
# Shell_TrayWnd (the one the hook installs into).
Add-Type -Namespace Win32 -Name NativeMethods -MemberDefinition @"
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
"@

function Get-TrayProcessId {
    $hwnd = [Win32.NativeMethods]::FindWindowW('Shell_TrayWnd', $null)
    if ($hwnd -eq [IntPtr]::Zero) { return 0 }
    # NB: the obvious name $pid is a read-only automatic variable in
    # PowerShell; any other spelling is fine.
    $trayProcId = 0
    [void][Win32.NativeMethods]::GetWindowThreadProcessId($hwnd, [ref]$trayProcId)
    return $trayProcId
}

function Test-HookResident([int]$ProcId) {
    if ($ProcId -eq 0) { return $true }  # no tray => cannot claim clean
    try {
        $proc = Get-Process -Id $ProcId -ErrorAction Stop
        return (@($proc.Modules | Where-Object {
                $_.ModuleName -eq 'TrayMirrorHook.dll' }).Count -gt 0)
    } catch {
        Write-Log "WARN module enumeration failed: $($_.Exception.Message)"
        return $true  # unknown, do not fail the run on a tooling error
    }
}

# --- start ----------------------------------------------------------------

if (-not (Test-Path $HostExe)) { throw "TrayMirror.exe not found: $HostExe" }
"" | Set-Content -Path $LogPath   # fresh log per run

Write-Log "=== TrayMirror stage 1 acceptance runbook ==="
Write-Log "host: $HostExe"
Write-Log "cycles: $Cycles x --duration $DurationSec"

# (1) The gate (spec 12.1): selftest in the host's own process, no
# Explorer contact, BEFORE any install. A failed selftest aborts.
& $HostExe --selftest | ForEach-Object { Write-Log "selftest: $_" }
if ($LASTEXITCODE -ne 0) {
    Write-Log "VERDICT: FAIL - selftest exit $LASTEXITCODE - no live contact happened"
    exit 1
}
Write-Log "selftest exit 0"

$TrayPid = Get-TrayProcessId
if ($TrayPid -eq 0) {
    Write-Log "VERDICT: FAIL - no Shell_TrayWnd (Explorer restarting?)"
    exit 1
}
Write-Log "explorer tray pid: $TrayPid"

$Baseline = Get-Process -Id $TrayPid
Write-Log ("baseline: handles={0} workingSet={1} MB" -f `
    $Baseline.HandleCount, [math]::Round($Baseline.WorkingSet64 / 1MB, 1))

# (2) The start/stop cycles.
$Failures = 0
for ($i = 1; $i -le $Cycles; $i++) {
    & $HostExe --duration $DurationSec 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $Failures++
        Write-Log "cycle $i : FAIL exit $LASTEXITCODE"
    }

    if (Test-HookResident $TrayPid) {
        $Failures++
        Write-Log "cycle $i : FAIL hook still resident in explorer"
    } else {
        $TrayAlive = [Win32.NativeMethods]::FindWindowW('Shell_TrayWnd', $null)
        if ($TrayAlive -eq [IntPtr]::Zero) {
            $Failures++
            Write-Log "cycle $i : FAIL Shell_TrayWnd is GONE (Explorer restart?)"
        }
    }

    if (($i % 10) -eq 0 -or $i -eq $Cycles) {
        $p = Get-Process -Id $TrayPid -ErrorAction SilentlyContinue
        if ($null -eq $p) {
            $Failures++
            Write-Log "cycle $i : FAIL explorer tray process is gone"
        } else {
            Write-Log ("cycle {0} : ok | handles={1} workingSet={2} MB" -f `
                $i, $p.HandleCount, [math]::Round($p.WorkingSet64 / 1MB, 1))
        }
    }
}

# (3) Leak verdict: first vs last sample. Handles drift naturally; the
# acceptance question is runaway growth, so the numbers are reported
# for the owner/agent to judge, with a generous sanity threshold.
$Final = Get-Process -Id $TrayPid -ErrorAction SilentlyContinue
$HandleGrowth = $Final.HandleCount - $Baseline.HandleCount
$WsGrowthMb = [math]::Round(($Final.WorkingSet64 - $Baseline.WorkingSet64) / 1MB, 1)
Write-Log ("summary: handleGrowth={0} workingSetGrowth={1} MB (drift is normal; runaway is a leak)" -f `
    $HandleGrowth, $WsGrowthMb)
if ($HandleGrowth -gt 2000) {
    $Failures++
    Write-Log "VERDICT-WARN: handle growth above 2000 - investigate before stage 2"
}

if ($SkipEmulator) {
    Write-Log "emulator phase skipped by request"
} else {
    # (4) NIM visibility: run the host, drive it with the stage 0
    # emulator (x64 and WOW64), then read the decoded records from the
    # host log. The emulator only calls Shell_NotifyIconW like any app.
    Write-Log "emulator phase: starting host for $EmuHostSec s"
    $hostProc = Start-Process -FilePath $HostExe `
        -ArgumentList "--duration", "$EmuHostSec" -PassThru
    Start-Sleep -Seconds 2   # let the hook install

    $emuRuns = @(
        @{ exe = $Emu64; tag = 'x64' },
        @{ exe = $Emu86; tag = 'x86' }
    )
    foreach ($e in $emuRuns) {
        if (-not (Test-Path $e.exe)) {
            Write-Log "WARN emulator missing: $($e.exe)"
            continue
        }
        & $e.exe add --count 3 --dur 4000 --tip "emu-$($e.tag)-add" | Out-Null
        & $e.exe modify --id 2 --dur 4000 --tip "emu-$($e.tag)-mod" | Out-Null
        & $e.exe setversion --id 3 --dur 4000 | Out-Null
        & $e.exe delete --id 1 --dur 4000 | Out-Null
        Write-Log "emulator $($e.tag): add/modify/setversion/delete issued"
    }

    Write-Log "emulator phase: waiting for the host to stop"
    Wait-Process -Id $hostProc.Id -Timeout ($EmuHostSec + 60) -ErrorAction SilentlyContinue

    # The host log holds this session (each host start truncates it).
    $nimLines = @(Get-Content $HostLog -ErrorAction SilentlyContinue |
        Where-Object { $_ -match 'NIM_' })
    Write-Log "emulator phase: host log shows $($nimLines.Count) NIM_* records"
    foreach ($line in ($nimLines | Select-Object -First 40)) {
        Write-Log "  $line"
    }
    if ($nimLines.Count -lt 8) {
        $Failures++
        Write-Log "VERDICT-WARN: fewer than 8 NIM records - the x64/x86 mix may be incomplete"
    }
}

# --- verdict --------------------------------------------------------------

if ($Failures -eq 0) {
    Write-Log "VERDICT: PASS - all cycles clean"
    Write-Log "log for analysis: $LogPath"
    exit 0
}
Write-Log "VERDICT: FAIL - $Failures failure(s), see log: $LogPath"
exit 1
