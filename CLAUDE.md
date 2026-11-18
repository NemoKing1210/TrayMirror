# CLAUDE.md

Project instructions for Claude Code. The canonical agent guidance lives in
`AGENTS.md`; read it first:

@AGENTS.md

## Claude-specific notes

- **Safety gate.** The default in this repository is *no live-Explorer contact*.
  Never run `TraySpikeCtl start`, `stop`, or `kill` against a real session unless
  the user explicitly asks for a live run in the current message. `selftest` is
  fine and is the expected verification for hook changes.
- **Verify before claiming done.** Run `scripts\build.ps1`, ctest (via the
  bundled CMake path, since ctest is not on `PATH`), and
  `TraySpikeCtl.exe selftest`; report the real output. Do not report success
  from memory or from an earlier run after changing code.
- **Language.** Discuss with the maintainer in the language they use (Russian is
  fine); everything committed — code, comments, docs, commit messages — is in
  English.
- **Plans and specs.** Local superpowers artifacts live under `superpowers/` and
  `.superpowers/`; they are gitignored and must never be committed. The project
  specification, status, and decision records are maintained locally and are not
  part of this repository.
- **Multi-step work.** For any change touching the hook, the record format, or
  the protocol, state the blast radius before editing: the hook runs inside
  `explorer.exe`, where a mistake can crash the shell.
- **At stage boundaries**, update the project status and add or update a decision
  record in the local decision log.
