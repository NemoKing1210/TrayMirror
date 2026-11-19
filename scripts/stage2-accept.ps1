# Stage 2 acceptance runbook (spec sections 9/10.2, plan task 12).
#
# *** OWNER-RUN ONLY ***
# The live phases start TrayMirror.exe without --selftest, which installs
# the hook into the LIVE Explorer session. Per the repo safety rules the
# agent never runs those phases; the project owner runs the script and
# shares the produced log for analysis.
#
# Emergency recovery, if the shell misbehaves:
#   taskkill /f /im explorer.exe; start explorer.exe
# The host's own kill switch: run `TrayMirror.exe --disable-hook` (arms
# Local\TrayMirror.Off; a resident hook goes inert forever).
#
# Criteria checked here (spec section 9, stage 2 acceptance):
#   1. ctest and the in-process selftest are green BEFORE any live contact;
#   2. one overlay strip per secondary monitor, each inside its monitor;
#   3. the initial synchronization surfaces pre-existing icons, and every
#      emulator NIM_* operation reaches the model;
#   4. a click on a mirrored icon is replayed to its owner (host log);
#   5. a hung owner never freezes the overlay UI;
#   6. the visual comparison against the primary tray is judged by the
#      owner and needs >= 8 of 10 application icons (spec 10.2).
#
# Usage:
#   powershell -File scripts\stage2-accept.ps1 -Phase selftest   # gate only
#   powershell -File scripts\stage2-accept.ps1                   # full run (~5 min)
#
# Phase selftest performs no live contact and is safe for an agent; the
# default full run is owner-executed.

param(
    [ValidateSet('selftest', 'all')][string]$Phase = 'all',
    [int]$OverlaySec = 30,
    [int]$SyncSec = 45,
    [int]$ClickWaitSec = 90
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$HostExe = Join-Path $Root 'build\src\host\TrayMirror.exe'
$Emu64 = Join-Path $Root 'build\spikes\s1\emu\TrayIconEmu.exe'
$Emu86 = Join-Path $Root 'build-x86\spikes\s1\emu\TrayIconEmu.exe'
$LogPath = Join-Path $Root 'build\stage2-accept.log'
$HostLog = Join-Path $env:LOCALAPPDATA 'TrayMirror\logs\host.log'
$Ctest = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'

$script:Failures = 0

function Write-Log([string]$Line) {
    $stamp = Get-Date -Format 'HH:mm:ss.fff'
    "$stamp $Line" | Tee-Object -FilePath $LogPath -Append
}

function Fail([string]$Line) {
    $script:Failures++
    Write-Log "FAIL: $Line"
}

Add-Type -Namespace Win32 -Name NativeMethods -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[StructLayout(LayoutKind.Sequential)]
public struct MONITORINFO {
    public int cbSize;
    public RECT rcMonitor;
    public RECT rcWork;
    public uint dwFlags;
}
public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
[DllImport("user32.dll")]
public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern int GetClassNameW(IntPtr hWnd, System.Text.StringBuilder name, int max);
[DllImport("user32.dll")]
public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")]
public static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint flags);
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFO info);
[DllImport("user32.dll")]
public static extern IntPtr SendMessageTimeoutW(IntPtr hWnd, uint msg, IntPtr wParam,
    IntPtr lParam, uint flags, uint timeout, out IntPtr result);
'@

# Every visible top-level window of the overlay class, regardless of monitor.
function Get-OverlayWindows {
    $script:OverlayWindows = @()
    $callback = [Win32.NativeMethods+EnumWindowsProc] {
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $name = New-Object System.Text.StringBuilder 64
        [void][Win32.NativeMethods]::GetClassNameW($hWnd, $name, 64)
        if ($name.ToString() -eq 'TrayMirrorOverlay') {
            $script:OverlayWindows += $hWnd
        }
        return $true
    }
    [void][Win32.NativeMethods]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:OverlayWindows
}

# An overlay must sit fully inside a NON-primary monitor, on its taskbar row.
function Test-OverlayPlacement([IntPtr]$Window) {
    $rect = New-Object Win32.NativeMethods+RECT
    if (-not [Win32.NativeMethods]::GetWindowRect($Window, [ref]$rect)) {
        Write-Log "  rect query failed for $Window"
        return $false
    }
    $monitor = [Win32.NativeMethods]::MonitorFromWindow($Window, 2) # NEAREST
    $info = New-Object Win32.NativeMethods+MONITORINFO
    $info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
    if (-not [Win32.NativeMethods]::GetMonitorInfoW($monitor, [ref]$info)) {
        Write-Log "  monitor query failed for $Window"
        return $false
    }
    $primary = ($info.dwFlags -band 1) -ne 0
    $inside = ($rect.Left -ge $info.rcMonitor.Left) -and
              ($rect.Right -le $info.rcMonitor.Right) -and
              ($rect.Top -ge $info.rcMonitor.Top) -and
              ($rect.Bottom -le $info.rcMonitor.Bottom)
    Write-Log ("  overlay {0}: ({1},{2})-({3},{4}) monitor ({5},{6})-({7},{8}) primary={9}" -f `
        $Window, $rect.Left, $rect.Top, $rect.Right, $rect.Bottom,
        $info.rcMonitor.Left, $info.rcMonitor.Top, $info.rcMonitor.Right,
        $info.rcMonitor.Bottom, $primary)
    if ($primary) {
        Write-Log '  a strip on the primary monitor is not the stage 2 contract'
        return $false
    }
    return $inside
}

function Wait-HostExit($Process, [int]$Seconds) {
    Wait-Process -Id $Process.Id -Timeout $Seconds -ErrorAction SilentlyContinue
    if (-not $Process.HasExited) {
        Write-Log "host did not exit in $Seconds s - stopping it"
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
}

function Get-LogLines([string]$Pattern) {
    return @(Get-Content $HostLog -ErrorAction SilentlyContinue |
        Where-Object { $_ -match $Pattern })
}

# --- start ----------------------------------------------------------------

if (-not (Test-Path $HostExe)) { throw "TrayMirror.exe not found: $HostExe" }
"" | Set-Content -Path $LogPath   # fresh log per run

Write-Log '=== TrayMirror stage 2 acceptance runbook ==='
Write-Log "phase: $Phase"
Write-Log "host: $HostExe"

# (1) The gate (spec 12.1): the suite and the in-process selftest, BEFORE
# any hook installation. A failure aborts without live contact.
if (Test-Path $Ctest) {
    & $Ctest --test-dir (Join-Path $Root 'build') --output-on-failure 2>&1 |
        ForEach-Object { Write-Log "ctest: $_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Log "VERDICT: FAIL - ctest exit $LASTEXITCODE - no live contact happened"
        exit 1
    }
    Write-Log 'ctest exit 0'
} else {
    Write-Log "WARN ctest not found at $Ctest - run it manually"
}

& $HostExe --selftest | ForEach-Object { Write-Log "selftest: $_" }
if ($LASTEXITCODE -ne 0) {
    Write-Log "VERDICT: FAIL - selftest exit $LASTEXITCODE - no live contact happened"
    exit 1
}
Write-Log 'selftest exit 0'

if ($Phase -eq 'selftest') {
    Write-Log 'VERDICT: PASS - selftest gate only; no live step was executed'
    Write-Log "log for analysis: $LogPath"
    exit 0
}

# --- (2) overlay presence --------------------------------------------------

Write-Log "phase 2: overlay presence (host for $OverlaySec s)"
$hostProc = Start-Process -FilePath $HostExe -ArgumentList '--duration', "$OverlaySec" -PassThru
Start-Sleep -Seconds 3   # the hook install and the first strips
$overlays = Get-OverlayWindows
Write-Log "overlay windows found: $($overlays.Count)"
if ($overlays.Count -eq 0) {
    Fail 'no TrayMirrorOverlay window exists on any secondary monitor'
} else {
    foreach ($window in $overlays) {
        if (-not (Test-OverlayPlacement $window)) {
            Fail "overlay $window is not inside its secondary monitor"
        }
    }
}
Wait-HostExit $hostProc ($OverlaySec + 30)

# --- (3) initial sync and emulator visibility ------------------------------

Write-Log "phase 3: initial sync + emulator visibility (host for $SyncSec s)"
$hostProc = Start-Process -FilePath $HostExe -ArgumentList '--duration', "$SyncSec" -PassThru
Start-Sleep -Seconds 2   # let the startup broadcast and the hook settle

foreach ($e in @(@{ exe = $Emu64; tag = 'x64' }, @{ exe = $Emu86; tag = 'x86' })) {
    if (-not (Test-Path $e.exe)) {
        Write-Log "WARN emulator missing: $($e.exe)"
        continue
    }
    & $e.exe add --count 3 --dur 3000 --tip "s2-$($e.tag)-add" | Out-Null
    & $e.exe modify --id 2 --dur 3000 --tip "s2-$($e.tag)-mod" | Out-Null
    & $e.exe delete --id 1 --dur 3000 | Out-Null
    Write-Log "emulator $($e.tag): add/modify/delete issued"
}

Write-Log 'the pre-existing application icons must now be visible on every overlay (owner check)'
Write-Log 'they appear because of the startup TaskbarCreated broadcast, not because the apps were touched'

Wait-HostExit $hostProc ($SyncSec + 30)
$nimLines = Get-LogLines 'NIM_'
Write-Log "host log shows $($nimLines.Count) NIM_* records"
foreach ($line in ($nimLines | Select-Object -First 30)) { Write-Log "  $line" }
if ($nimLines.Count -lt 6) {
    Fail "fewer than 6 NIM_ records - the model did not see the emulators"
}

# --- (4) click checklist (owner) -------------------------------------------

Write-Log "phase 4: click checklist (host for $($ClickWaitSec + 30) s)"
Write-Log 'Owner, please use the icon "s2-click" that the script is about to create on the overlay:'
Write-Log '  1) left-click it twice with a pause between the clicks'
Write-Log '  2) double-click it'
Write-Log '  3) right-click it (a context menu should open, if the emulator had one)'
Write-Log '  4) hover it (the tooltip must show "s2-click")'
$hostProc = Start-Process -FilePath $HostExe -ArgumentList '--duration', "$($ClickWaitSec + 30)" -PassThru
Start-Sleep -Seconds 2
$emuJob = Start-Process -FilePath $Emu64 -ArgumentList 'add', '--tip', 's2-click', '--dur', "$ClickWaitSec" -PassThru

$deadline = (Get-Date).AddSeconds($ClickWaitSec)
$dispatched = @()
while ((Get-Date) -lt $deadline) {
    $dispatched = Get-LogLines 'dispatch:'
    if ($dispatched.Count -ge 4) { break }
    Start-Sleep -Seconds 2
}
Write-Log "host log shows $($dispatched.Count) dispatched calls"
foreach ($line in $dispatched) { Write-Log "  $line" }
if ($dispatched.Count -lt 4) {
    Fail "fewer than 4 dispatched calls - the click checklist did not complete"
}
Wait-HostExit $emuJob 10
Wait-HostExit $hostProc ($ClickWaitSec + 60)

# --- (5) hung owner ---------------------------------------------------------

Write-Log 'phase 5: hung owner (the emulator stops pumping for 20 s)'
$hostProc = Start-Process -FilePath $HostExe -ArgumentList '--duration', '60' -PassThru
Start-Sleep -Seconds 2
$hangJob = Start-Process -FilePath $Emu64 -ArgumentList 'add', '--tip', 'TRAYHANG', '--dur', '60000', '--hang', '20000' -PassThru
Start-Sleep -Seconds 2
Write-Log 'Owner: while TRAYHANG is hung, click its overlay icon (once is enough)'
Write-Log 'the overlay must stay responsive: the script now measures WM_NULL reply time'

$responsive = $false
foreach ($window in (Get-OverlayWindows)) {
    $result = [IntPtr]::Zero
    $ok = [Win32.NativeMethods]::SendMessageTimeoutW($window, 0, [IntPtr]::Zero,
        [IntPtr]::Zero, 2, 500, [ref]$result)   # WM_NULL, SMTO_ABORTIFHUNG, 500 ms
    if ($ok -ne [IntPtr]::Zero) {
        $responsive = $true
        Write-Log "overlay $window answered WM_NULL within 500 ms"
    } else {
        Fail "overlay $window did not answer WM_NULL within 500 ms"
    }
}
if (-not $responsive) { Fail 'no overlay answered while the owner was hung' }

Wait-HostExit $hangJob 10
Wait-HostExit $hostProc 90

# --- (6) visual comparison (owner) -----------------------------------------

Write-Log 'phase 6: composition checklist (owner judgement, spec 10.2)'
Write-Log 'Compare each overlay strip with the primary tray, ignoring the system icons'
Write-Log '(network, volume, battery, clock and the chevron, which stage 2 does not mirror).'
$answer = Read-Host 'How many of the 10 ordinary application icons are correct? (0-10)'
$score = 0
if (-not [int]::TryParse($answer, [ref]$score)) {
    Fail "the checklist answer '$answer' is not a number"
} elseif ($score -lt 8) {
    Fail "only $score of 10 application icons are correct (spec 10.2 needs 8)"
} else {
    Write-Log "owner score: $score/10"
}

# --- verdict ----------------------------------------------------------------

if ($script:Failures -eq 0) {
    Write-Log 'VERDICT: PASS - stage 2 acceptance criteria met'
    Write-Log "log for analysis: $LogPath"
    exit 0
}
Write-Log "VERDICT: FAIL - $($script:Failures) failure(s), see log: $LogPath"
exit 1
