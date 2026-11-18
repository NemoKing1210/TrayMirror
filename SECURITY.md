# Security Policy

## Project status

TrayMirror is a **research-stage project**: stage 0 (feasibility spikes). There
are no releases yet, and the stage 0 code is not intended for production use.
It nevertheless runs code inside `explorer.exe`, so security reports are welcome
and taken seriously.

| Version | Supported |
|---|---|
| `main` (stage 0, pre-release) | Reports accepted, fixes best-effort |
| Released builds | None yet |

## Reporting a vulnerability

**Do not open a public issue, pull request, or discussion for a vulnerability.**

Preferred channel — GitHub private vulnerability reporting:

1. Open the repository on GitHub.
2. Go to **Security → Advisories → Report a vulnerability**.
3. Describe the issue and, if possible, include a minimal reproduction.

If private reporting is unavailable, contact the maintainer via
[github.com/NemoKing1210](https://github.com/NemoKing1210) and ask for a private
channel. Do not include vulnerability details in a public message.

Please include:

- Windows build (`winver`) and architecture;
- which component is affected (`TrayMirrorSpikeHook.dll`, `TraySpikeCtl`,
  the emulator/dump tools, or the repository itself);
- whether the spike hook was installed in a live Explorer at the time;
- steps to reproduce and the observed impact;
- relevant log excerpts, **redacted** — see the privacy warning below.

This is a single-maintainer project without a bug bounty. Reports are handled
best-effort; no response time is guaranteed. Coordinated disclosure is expected:
give the maintainer a reasonable opportunity to fix the issue before publishing
details.

## Scope

In scope:

- **Hook DLL safety** — crashes, memory-safety issues, undefined behavior, or
  payload handling that can destabilize `explorer.exe`.
- **IPC objects** — `Local\TrayMirrorSpike.Off`, `Local\TrayMirrorSpike.Stop`,
  `Local\TrayMirrorSpike.Ctl`, the pid and log files in `%TEMP%`; handle and
  path hygiene around them.
- **Callback dispatch** — message spoofing, focus/UIPI interactions, hangs or
  resource exhaustion caused by a malicious or misbehaving icon owner.
- **Tooling** — parser bugs in `TraySpikeDump` reachable from untrusted log
  files, path handling in `scripts/build.ps1`.
- **Repository-level issues** — leaked credentials, unsafe defaults, CI supply
  chain (when CI exists).

Out of scope:

- Anything that requires Administrator/SYSTEM privileges to exploit.
- Deliberate, documented limitations of the project (for example,
  `Shell_NotifyIconGetRect` returning primary-monitor coordinates).
- A process running as the **same user** signaling the kill switch or opening
  the hook log. The kill switch is a safety mechanism, not an authorization
  boundary; same-user processes are already mutually trusted on Windows.
- Vulnerabilities in third-party applications that TrayMirror merely displays.
- Physical access, social engineering, and denial of service against the
  maintainer.

## Privacy warning about logs

Spike logs (`%TEMP%\TrayMirrorSpike.log`, `%TEMP%\TrayMirrorEmu.log`) contain raw
tray traffic: window handles, icon identifiers, tooltips, and executable paths of
software the user runs. Tooltips can contain personal data (message counters,
account names, file names). Before sharing a log in a report:

- reproduce with the emulator (`TrayIconEmu.exe`) instead of real applications
  where possible;
- remove or mask real application tooltips and paths;
- share only the records needed to demonstrate the issue.

## Safety notes for researchers

- Do not install the hook into an Explorer session you do not own, and do not
  run live-Explorer experiments on someone else's machine.
- Run `TraySpikeCtl.exe selftest` before any hook installation; it verifies the
  kill switch without contacting Explorer.
- Emergency recovery: `taskkill /f /im explorer.exe; start explorer.exe`.
- If your report is about the hook destabilizing Explorer, say so explicitly in
  the first line so it is triaged as a stability issue.
