# Contributing to TrayMirror

Thanks for your interest. TrayMirror is a pre-release, stage-gated project: the
rules below are stricter than in a typical repository because part of the code
runs inside `explorer.exe`.

Read before your first change:

1. [`AGENTS.md`](AGENTS.md) — the working rules (applies to humans and AI agents).
2. [`SECURITY.md`](SECURITY.md) — how to report vulnerabilities privately.

The project specification and status are maintained by the maintainer locally
and are not part of this repository; ask for the current versions when you need
details they contain.

## Project status and stage gates

The project is at **stage 0 — research spikes**. Stage 1 work does not start
until the maintainer records the stage 0 feasibility decision.

- Do not open pull requests that jump ahead of the current stage; discuss the
  proposal in an issue first.
- Runs on a **live Explorer** (`TraySpikeCtl start/stop/kill`) are performed only
  by the project owner. Pull requests must not require them for validation.
- The hook selftest (`TraySpikeCtl.exe selftest`) is the expected verification
  for any hook change.

## Ways to contribute

- **Bug reports** — exact Windows build, architecture, steps to reproduce,
  expected vs. observed.
- **Compatibility reports** — application name and version, click / right click
  / menu / tooltip / balloon status. These feed the compatibility table kept for
  stage 3.
- **Spike measurements** — see "Recording results" below.
- **Documentation** — corrections, clarifications, translations of user-facing
  docs (the repository itself stays English).
- **Code and tests** — after the stage gate allows it.

For anything larger than a typo or a test fix, open an issue first and describe
the approach. It is cheaper than reviewing a rejected PR.

## Development setup

| | |
|---|---|
| OS | Windows 11, x64 (reference build: 10.0.26300) |
| Toolchain | Visual Studio Build Tools 2026 (MSVC, bundled CMake + Ninja) |
| Shell | PowerShell |

CMake, Ninja, and ctest are **not** on `PATH`. Use the build script; it locates
the Build Tools installation and initializes the environment.

```powershell
scripts\build.ps1                 # x64 → build\
scripts\build.ps1 -Arch win32     # x86 → build-x86\ (WOW64 emulator only)
scripts\build.ps1 -Config Release

$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$cmake\ctest.exe" --test-dir build --output-on-failure

.\build\spikes\s1\ctl\TraySpikeCtl.exe selftest
```

The build is MSVC-only and uses `/W4 /WX`; a warning fails the build. Do not
weaken warning levels or suppress warnings to make a change compile.

## Coding guidelines

- C++20, Win32 APIs, no third-party runtime unless approved in an ADR.
- RAII for every handle; no bare `new`/`delete`.
- Prefer clarity and small functions; comments explain *why*, not *what*.
- The hook DLL is special (spec §2.3, §12.4): no STL, no heap allocations, no C++
  exceptions, no CRT file functions, no blocking synchronization, kernel32 only.
  Changes here need a clear safety argument and a passing selftest.
- No new periodic activity (timers, polling threads) without an ADR justifying it.
- Never introduce `CreateRemoteThread`, manual injection, Explorer memory
  patching, or undocumented Explorer COM interfaces.
- Structures and message sequences are established by measurement, never by
  assumption. Record the evidence.

## Tests

- Add or update unit tests for any change to the record format, parser, model
  rules, or config handling.
- `ctest` must be green; `TraySpikeCtl.exe selftest` must be green for hook and
  record-format changes.
- Report the actual test output in the pull request; do not claim a pass you did
  not run.
- Fuzzing targets and stress scenarios are described in the spec (§10); add
  cases when you touch the parser.

## Recording results

- Report spike measurements in the issue or pull request with an explicit
  verdict: **GO**, **NO-GO**, or **GO with caveats**.
- Significant decisions are recorded in the project's local decision log. Do not
  silently reverse an existing decision — raise it in an issue and supersede it
  explicitly.
- Update the project status at stage boundaries and whenever deferred work
  changes.
- Keep observed raw data (for example, a tooltip captured on a non-English
  system) as measured, with a translation in parentheses.

## Commits and pull requests

The repository uses Conventional Commit-style subjects:

```
feat(spike): TrayIconEmu icon emulator and message receiver
fix(spike): SEH guard for lpData
docs(spike): S1 report — GO
chore: repo bootstrap, spec, build scaffolding
```

- One task per commit; keep pull requests small and focused.
- Subject in the imperative mood, lowercase after the prefix.
- All committed text — code, comments, docs, commit messages — is in English.
- Do not commit build outputs, logs, dumps, `superpowers` folders, or editor/OS
  junk; `.gitignore` covers them.
- Never commit secrets, credentials, tokens, or `.env` files.
- A pull request description should state: what changed, why, how it was tested,
  and which decision records or status documents are affected.

## Review

Reviewers check, in this order: safety of the hook path, correctness and tests,
adherence to the spec and ADRs, documentation updates, and style. Safety-critical
changes may be held until a spike measurement or an owner-approved live run
confirms the behavior.

## Security

Never report a vulnerability in a public issue or pull request. Follow
[`SECURITY.md`](SECURITY.md). Test the hook only on machines you own, and only
after the selftest passes.

## Licensing

The project has not adopted a license yet; one will be added before the first
public release. Until then, please open an issue before submitting substantial
code so licensing can be settled first. Small documentation fixes are welcome at
any time.

## Conduct

Be respectful and constructive. Technical disagreement is fine; personal attacks,
harassment, and low-effort hostility are not. Maintainers may close issues or
pull requests that ignore the rules above.
