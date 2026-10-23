# Changelog

## 0.4.0

### Verification status (read this first)

- The platform-independent core (`src/core/`: argument parsing, ring buffer, statistics, trigger logic, event
  classification, findings engine, CSV/JSON output, retention selection) has **87 assertions** across the suite in `tests/test_core.c`
  that pass under AddressSanitizer/UBSan with `-Wall -Wextra -Wpedantic -Wshadow` clean.
- The Windows layer (`src/win/`, `src/main.c`) was written and syntax/type-checked against stand-in headers only.
  It has **not yet been compiled against the real Windows SDK or run on Windows**. Expect to fix a few compile
  errors on the first real build, and run through a manual test pass (see "Suggested first-run checks" below)
  before trusting it for an unattended session.

### Fixed

- **Likely crash at start-up:** the 600-sample ring was a ~2 MiB local array in `wWinMain`, larger than the default
  1 MiB (MSVC) / 2 MiB (MinGW) stack. It is now heap-allocated and sized to the configured history.
- Manual hotkey captures were silently dropped inside the 60 s automatic cooldown; they now bypass it (3 s rate limit).
- Unavailable counters were stored and reported as `0`; they are now "unavailable" and never produce findings.
- The "max over the whole ring" analysis could report an unrelated spike from minutes earlier; analysis now compares
  a lead-in window against the baseline.
- Any `WHEA`/`Display`/`disk` substring anywhere in 30 minutes of raw XML was reported as `[HIGH]`; events are now
  parsed (provider, ID, level, time) and weighted by distance from the trigger.
- `SendMessageTimeout` with `SMTO_ABORTIFHUNG` can fail without `ERROR_TIMEOUT`, missing real hangs; the result is
  now cross-checked with `IsHungAppWindow`. UIPI denials and the secure desktop no longer count toward hang streaks.
- Process CPU % used the nominal window instead of the measured one, and PID reuse could produce bogus deltas.
- `_snwprintf` could leave unterminated strings on truncation; all formatting goes through terminating helpers.
- CSV writer converted one character at a time (slow) and mangled surrogate pairs.
- Storage descriptor strings were read without a bounds check.
- Scheduled task created by `schtasks /Create` had Windows' defaults (stop after 3 days, no start/stop on battery).
- `taskkill /IM` in the (un)installer could affect other users' instances; now filtered to the current user.
- MinGW builds lacked `-municode` for `wWinMain`.
- Drive enumeration touched network/optical drives, which can block a capture for tens of seconds.
- Fallback report path was the current directory (often read-only for a scheduled task); now `%TEMP%`.

### Security / hardening

- Owner-only protected ACL on report folders; unique incident folders; retention (`--max-reports`) with a safe,
  whitelist-based deleter; free-space guard (`--min-free-mb`).
- Minidump: least privilege, deny-list (lsass etc.), self-skip, deadline cancel callback, `CREATE_NEW` with no sharing.
- CSV formula-injection neutralisation; control-character stripping; `--redact` mode.
- System32-only DLL search, heap-corruption termination, no critical-error dialogs, strict argument validation,
  size-capped log, graceful shutdown on logoff, crash filter that logs and lets the task restart the recorder.
- Process queries use `PROCESS_QUERY_LIMITED_INFORMATION` (no `PROCESS_VM_READ`) except for the minidump itself.

### Added

- Per-CPU and per-disk sampling (worst DPC/interrupt/load core, worst disk latency/queue with names), CPU performance %.
- Probe status/latency per sample; suspend/resume awareness; `--help`/`--version`; `--lead-window`, `--max-reports`,
  `--min-free-mb`, `--dump-timeout-ms`, `--redact`, `--no-hotkey`, `--name=value` syntax.
- Findings engine with confidence, evidence, baseline comparison and next steps; `summary.json`.
- `events_digest.csv` (classified, with age relative to the trigger; includes LiveKernelEvent and sleep/resume).
- `foreground.txt`, per-thread CPU and names, wait-chain status names and cross-process blocker attribution,
  top CPU/I-O processes with I/O and page-fault rates, `power.txt`, driver versions/vendors, GPU driver
  version/VRAM, HDD/SSD detection, capture step timings.

### Changed

- `main.c` split into `src/core/` (portable, tested) and `src/win/` (Windows API) modules.
- Installer registers the task via `register_task.ps1` (no time limit, runs on battery, restart on failure).

### Suggested first-run checks on Windows

1. Build with MSVC and/or MinGW; run `ctest`.
2. `WinFreezeTrace.exe --help` from a terminal; confirm startup doesn't crash and `winfreezetrace.log` appears.
3. Press Ctrl+Alt+F12 with a normal app focused; inspect every file in the new incident folder.
4. Suspend an app (Process Explorer/`pssuspend`) in the foreground for ~1 s to trigger a hang capture; confirm the
   wait chain / probe status makes sense and `target.dmp` opens in WinDbg.
5. Sleep and resume the PC; confirm no false stall capture and a `sleep_resume` note in the log.
6. Run `install_task.cmd`, then check Task Scheduler: no 3-day limit, runs on battery.
7. Try `--redact` and `--max-reports 2` and confirm masking and pruning.
