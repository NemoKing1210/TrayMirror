# TrayMirror

**Native system tray on every monitor in Windows 11.**

TrayMirror mirrors the notification area (tray) from the primary taskbar onto
secondary monitors, aiming to be indistinguishable from the original: icons,
tooltips, clicks, context menus and balloons.

> **Project status: stage 1 (skeleton) — code complete, acceptance runs
> pending.**
> Spikes S1–S3 returned **GO** (stage 0 closed). Stage 1 built the
> versioned ring protocol, the production hook DLL, the host process
> with selftest, the dump tool, CI and a deterministic parser fuzz
> corpus — ctest 6/6 green. The 100× live start/stop acceptance and the
> first GitHub push are next. See the [roadmap](#roadmap).

---

## The problem

On Windows 11, application tray icons live only on the primary monitor's
taskbar. Secondary taskbars expose application buttons and a clock, but no
tray. Users with multi-monitor setups have to move the cursor to the primary
screen for every tray interaction.

## How it works

```
Application --Shell_NotifyIcon--> WM_COPYDATA --> Shell_TrayWnd (explorer.exe)
                                                       |
                        HookDll (WH_CALLWNDPROCRET, inside explorer.exe)
                                                       |
                        shared memory ring buffer + named event   (stage 1)
                        %TEMP%\TrayMirrorSpike.log                 (stage 0)
                                                       |
                                         Host (separate process)
                                                       |
                        TrayModel --> Renderer --> overlay per monitor
                                                       |
                 click/hover --> Dispatcher --> callback to icon owner
```

Design rules that constrain every change:

- the code inside Explorer is minimal and allocation-free;
- only public, documented mechanisms are used (no injection, no patching);
- the Hook ↔ Host protocol is versioned;
- any abnormal condition degrades to safe mode instead of hanging Explorer.

## Repository layout

```
src/
  proto/           ring record format, CRC32, SPSC writer/reader, tray decoder
  hook/            TrayMirrorHook.dll — production hook (in explorer.exe)
  host/            TrayMirror.exe — model owner, install, reader thread, selftest
  host/ipc/        shared ring (DACL, adopt-on-exists), single instance
  tools/           TrayMirrorDump.exe — passive ring snapshot dump
spikes/            stage 0 research code (kept for reference tools)
scripts/
  build.ps1        MSVC build driver (x64 / x86)
  stage1-accept.ps1   stage 1 acceptance runbook (owner-run, live Explorer)
tests/             unit (GoogleTest), integration, deterministic fuzz corpus
```

## Requirements

| | |
|---|---|
| OS | Windows 11, x64 (reference build: 10.0.26300) |
| Toolchain | Visual Studio Build Tools 2026 (MSVC, bundled CMake + Ninja) |
| Shell | PowerShell |

CMake, Ninja and ctest are **not** on `PATH`; [`scripts/build.ps1`](scripts/build.ps1)
wires them up for the build.

Dependencies (spdlog, GoogleTest) are managed by [vcpkg](https://vcpkg.io)
manifest mode: on first use `scripts\build.ps1` clones and bootstraps vcpkg
into a git-ignored `.vcpkg\` directory, then passes its CMake toolchain —
no manual installation is needed.

## Build

```powershell
scripts\build.ps1                 # x64 → build\
scripts\build.ps1 -Arch win32     # x86 → build-x86\ (WOW64 spikes only)
scripts\build.ps1 -Config Release
```

The build is MSVC-only with `/W4 /WX` (warnings are errors). The hook and the
controller are x64-only — a 32-bit hook cannot be installed into a 64-bit
Explorer. Only the icon emulator needs an x86 build, to emulate 32-bit
`Shell_NotifyIcon` senders.

## Tests

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$cmake\ctest.exe" --test-dir build --output-on-failure

.\build\src\host\TrayMirror.exe --selftest       # production selftest, no Explorer contact
.\build\spikes\s1\ctl\TraySpikeCtl.exe selftest   # stage 0 hook selftest
```

The selftest exercises the hook DLL in its own process, including the
kill-switch and SEH scenarios. It must pass **before** the hook is ever
installed into a live Explorer (spec 12.1). The suite includes a
deterministic fuzz corpus over the host's record parser (200k
edge-biased garbage buffers, ~5 s).

## Stage 1 binaries

All production binaries land under `build\`:

| Binary | Purpose |
|---|---|
| `src\host\TrayMirror.exe` | The host: no args runs until Ctrl+C; `--duration <sec>`, `--selftest`, `--diagnostic`, `--log-level`, `--disable-hook` (arms the kill switch `Local\TrayMirror.Off` and exits) |
| `src\hook\TrayMirrorHook.dll` | The production hook DLL — installed per-thread on `Shell_TrayWnd`, writes tray records into the shared ring |
| `src\tools\TrayMirrorDump.exe` | Passive ring dump: `[--mode records\|summary] [--limit N]`; opens the mapping read-only, parses one private snapshot |

Host log: `%LOCALAPPDATA%\TrayMirror\logs\host.log` (truncated per
run). Exit codes: 0 ok, 2 already running, 3 no tray window, 4 install
failure, 5 selftest failed.

## Stage 0 tools

All binaries land under `build\spikes\`:

| Tool | Purpose |
|---|---|
| `s1\ctl\TraySpikeCtl.exe` | Installs/removes the hook, reports status, selftest, broadcasts `TaskbarCreated` |
| `s1\emu\TrayIconEmu.exe` | Emulates applications: adds/modifies/deletes icons, receives callbacks, logs anchors |
| `s1\dump\TraySpikeDump.exe` | Dumps the spike log (`words`) or summarizes it (`stats`) |

```powershell
.\build\spikes\s1\ctl\TraySpikeCtl.exe selftest
.\build\spikes\s1\ctl\TraySpikeCtl.exe start     # hooks Shell_TrayWnd, runs until stop
.\build\spikes\s1\ctl\TraySpikeCtl.exe status
.\build\spikes\s1\ctl\TraySpikeCtl.exe stop      # clean removal

.\build\spikes\s1\emu\TrayIconEmu.exe add --count 3 --tip S1ADD --dur 60000
.\build\spikes\s1\emu\TrayIconEmu.exe modify --id 1 --v4
.\build\spikes\s1\emu\TrayIconEmu.exe recv --dur 60000

.\build\spikes\s1\dump\TraySpikeDump.exe --mode stats
.\build\spikes\s1\dump\TraySpikeDump.exe --mode words --limit 5
```

The emulator log is `%TEMP%\TrayMirrorEmu.log`; the hook log is
`%TEMP%\TrayMirrorSpike.log`.

## Safety

The stage 0 hook is installed **into explorer.exe**. Two independent guards:

- **Kill switch** — `Local\TrayMirrorSpike.Off` is checked by the hook on every
  call. Set it with `TraySpikeCtl kill`; the hook then no-ops until unloaded.
- **Selftest** — must be green before installing the hook into a live Explorer.

Emergency Explorer recovery:

```powershell
taskkill /f /im explorer.exe; start explorer.exe
```

Removal: `TraySpikeCtl stop` (clean, unloads the DLL) or `TraySpikeCtl kill`
(emergency, leaves the hook inert). During stage 0 the hook writes records
directly to `%TEMP%\TrayMirrorSpike.log` instead of using the production ring
buffer; the stage 1 protocol removes that blocking write from the tray thread.

## Roadmap

| Stage | Scope | Status |
|---|---|---|
| 0 | Research spikes S1–S9, feasibility decision | done — S1–S3 GO, verdict GO (S4–S9 folded into later stages) |
| 1 | Skeleton: protocol, hook, host, CI, logging | code complete — 100× acceptance runs pending (owner) |
| 2 | Icon model, first overlay rendering | planned |
| 3 | Native behavior: v4 protocol, menus, balloons, overflow | planned |
| 4 | Reliability: Explorer restarts, sleep, RDP, monitors, stress | planned |
| 5 | Settings, drag-and-drop, per-app rules | planned |
| 6 | Optimization, security, signing, installer, release | planned |
| 7 | Win11 system icons (optional) | optional |

Stage gates are strict: the next stage starts only after the previous stage's
acceptance criteria are met.

## Project documents

- [Contributing guide](CONTRIBUTING.md) — setup, stage gates, review process
- [Security policy](SECURITY.md) — private vulnerability reporting and scope
- [Agent instructions](AGENTS.md) — rules for AI coding agents

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full process and
[`SECURITY.md`](SECURITY.md) for private vulnerability reporting.

- Work strictly by stages: do not start the next stage before the current
  stage's acceptance criteria are met.
- One task per commit, with tests and documentation updates.
- Never invent undocumented structures or message sequences — measure them and
  include the evidence in the issue or pull request.
- All repository artifacts (code comments, docs, commit messages, CMake
  messages) are written in English.
- Working with an AI agent? Start from [`AGENTS.md`](AGENTS.md).

## License

No license has been chosen yet. A license will be added before the first public
release. TrayMirror is not affiliated with Microsoft.
