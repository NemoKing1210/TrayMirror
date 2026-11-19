# AGENTS.md

Guidance for AI coding agents working in this repository. Read this before
changing anything. The authoritative requirements and the current project status
are maintained by the project owner outside this repository — ask for them when
a task depends on details not captured here, and never guess.

## What this project is

TrayMirror mirrors the Windows 11 notification area (tray) onto secondary
monitors. The architecture is a minimal hook DLL inside `explorer.exe`
(`WH_CALLWNDPROCRET` on the `Shell_TrayWnd` thread) plus a separate Host
process that owns the model, rendering, and callbacks to icon owners.

The project has completed **stage 0 (research spikes, feasibility GO —
ADR-0002)** and **stage 1 (skeleton — protocol, production hook, host,
dump tool; acceptance PASS, ADR-0004)**. The next stage is **stage 2 —
model and first rendering** (spec §9). Stage 0's spike tooling remains in
`spikes/` as historical research; the production code lives in `src/`.

## Ground truth, in order

1. The project specification (maintained locally by the owner): requirements,
   architecture, stage gates, acceptance criteria, risk register, working rules.
2. The project status (local): what is done, what is deferred, and why.
3. The decision log: decisions already made. Do not silently reverse one;
   supersede it with a new decision.
4. The spike reports: measured facts (layouts, message sequences). Measurements
   beat assumptions; memory beats neither.

If code and the specification disagree about intent, the specification wins.
Surface the conflict to the owner and update the affected document in the same
change.

## Hard safety rules

- **Never install a hook into a live Explorer** — do not run the production
  host (`TrayMirror.exe` with no arguments), `TraySpikeCtl start`, `stop`, or
  `kill` against a real session unless the project owner explicitly asks for it
  in the current task. Live-Explorer acceptance runs are owner-executed via
  `scripts\stage1-accept.ps1`.
- `TrayMirror.exe --selftest` and `TraySpikeCtl selftest` are safe and
  expected: they exercise the hook DLL in their own process without contacting
  Explorer.
- The kill switch (production: `Local\TrayMirror.Off`; spike:
  `Local\TrayMirrorSpike.Off`) must be verified by selftest **before** any hook
  installation. This is a spec rule, not a suggestion.
- Emergency Explorer recovery (only if the owner asks for live testing):
  ```powershell
  taskkill /f /im explorer.exe; start explorer.exe
  ```
- `HookDll` constraints (spec §2.3, §12.4): no STL, no `new`/`malloc`, no C++
  exceptions, no CRT file functions, no blocking synchronization, kernel32 only.
  Any change here requires extra scrutiny and a passing selftest.
- Do not invent structures. `TRAYDATA` layouts, `NOTIFYICONDATA` offsets, and
  click message sequences must be measured and written down in the spike reports.
- Never use `CreateRemoteThread`, manual injection, Explorer memory patching, or
  undocumented Explorer COM interfaces.

## Build and test

CMake, Ninja, and ctest are **not in `PATH`**. `scripts/build.ps1` hardcodes the
Visual Studio Build Tools 2026 location; if it is missing, the script fails
loudly — fix the environment, do not bypass the script.

```powershell
scripts\build.ps1                 # x64 → build\
scripts\build.ps1 -Arch win32     # x86 → build-x86\ (WOW64 emulator only)
scripts\build.ps1 -Config Release

$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$cmake\ctest.exe" --test-dir build --output-on-failure

.\build\src\host\TrayMirror.exe --selftest      # production: hook + ring in-process
.\build\spikes\s1\ctl\TraySpikeCtl.exe selftest # spike tooling, if touched
```

The build is MSVC-only with `/W4 /WX`: a warning is a build failure. The
production hook and host are x64-only; only the emulator/dump tools make sense
as x86 builds.

Before claiming a change is done, run the build, `ctest`, and the selftest, and
report their actual output. Never claim success unverified.

## Repository map

| Path | Contents |
|---|---|
| `src/hook/` | `TrayMirrorHook.dll` — the only production code that runs in Explorer |
| `src/host/` | `TrayMirror.exe` — host: install, reader thread, model, logging, selftest, clean shutdown; IPC in `ipc/` |
| `src/proto/` | Record format, ring writer/reader, CRC, tray payload decode — shared by hook and host |
| `src/tools/` | `TrayMirrorDump.exe` — passive snapshot dump of the production ring |
| `tests/unit/` | Unit tests for proto, ring, hook, host, dump, shared ring, payload |
| `tests/integration/` | Selftest integration and hook-latch tests |
| `tests/fuzz/` | Deterministic parser fuzz (x64; runs under ctest) |
| `spikes/common/` | Stage 0 record format (`Record.h`), IPC names (`TraySpike.h`), unit test |
| `spikes/s1/hook/` | `TrayMirrorSpikeHook.dll` — stage 0 spike hook |
| `spikes/s1/ctl/` | `TraySpikeCtl.exe` — start/stop/kill/status/selftest/broadcast |
| `spikes/s1/emu/` | `TrayIconEmu.exe` — emulates applications, receives callbacks |
| `spikes/s1/dump/` | `TraySpikeDump.exe` — log dump (`words`) and summary (`stats`) |
| `scripts/build.ps1` | The only supported build entry point |
| `scripts/stage1-accept.ps1` | Owner-run stage 1 acceptance runbook (live Explorer) |
| `.github/workflows/ci.yml` | CI: build, ctest, selftest |
| `vcpkg.json` | Dependencies (spdlog, gtest) |
| `AGENTS.md` | These rules; the canonical agent guidance |
| `CONTRIBUTING.md` | Contribution process, stage gates, review expectations |
| `SECURITY.md` | Private vulnerability reporting, scope, privacy warning about logs |

## Working conventions

- Work strictly by stages. Do not start a stage before the previous stage's
  gate is closed and recorded (stage 1 closed — ADR-0004; stage 2 next).
- One task per commit, with tests and documentation updates.
- Record every significant decision in the project decision log; record spike
  measurements in the spike reports; update the project status at stage
  boundaries. These documents are maintained locally and are not part of this
  repository.
- **All committed text is English** — code comments, docs, CMake messages, script
  comments, commit messages. Raw observed data (e.g. a tooltip captured on a
  non-English system) may stay in its original language with a translation in
  parentheses, because it is measurement data.
- Superpowers working artifacts (`superpowers/`, `.superpowers/`) are local and
  gitignored. Never commit them.
- Do not commit build outputs, logs, dumps, or editor/OS junk; `.gitignore`
  covers them.
- Never commit secrets, credentials, or `.env` files.
- Repository author/maintainer: [NemoKing1210](https://github.com/NemoKing1210).
- Code style: C++20, RAII for every handle, no bare `new`/`delete`, no
  suppressions of `/W4` warnings.
- Performance: any new periodic activity (timer, polling thread) must be
  justified in an ADR; the default answer is no.

## Done means

1. The change matches the spec (or the spec was updated deliberately).
2. Tests cover the new behavior; `ctest` is green.
3. The selftest is green if the hook or record format changed.
4. The decision log, project status, and spike reports reflect reality.
5. No warnings, no new allocations on the hook path, no live-Explorer contact.
