# scripts/ — verification notes

Guidance that only matters when you are editing something in this directory. The
repo-wide rules live in the root [CLAUDE.md](../CLAUDE.md).

## A `.ps1` is only verified by RUNNING it under `powershell.exe`

The default Windows shell is still PowerShell 5.1, which reads a BOM-less script in the
machine's ANSI codepage (1252 on a Western install) rather than UTF-8. An em dash's third
byte decodes to U+201D, which PowerShell honours as a double-quote delimiter: one em dash
inside a double-quoted string truncates it, the quote meant to close it opens a runaway
string that swallows the lines below, and the parse error is reported against an innocent
line further down.

`scripts/setup.ps1` was therefore unable to start from M0.4 (2026-06-17) until 2026-08-31,
while its own header claimed CI exercised it — the Windows job runs `build.ps1` under
`shell: pwsh` (PowerShell 7, UTF-8 regardless) and no job runs `setup.ps1` at all.

`.ps1` files are ASCII-only now and the lint job greps for it. The trap within the trap:
`[Parser]::ParseFile()` reports ZERO errors on a file `powershell.exe -File` refuses to
run, because it decodes as UTF-8 — a parse check is not the proof, the same way grepping a
build log is not the proof. Run it.
