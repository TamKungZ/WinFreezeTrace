# Changelog

## 0.6.0

### Changed (split: recorder + Manager)

- **The tray icon, balloons, menu and Start with Windows moved out of the recorder** into a new, separate WinUI 3 program,
  **WinFreezeTrace Manager** (`manager/`). The recorder now only monitors and captures.
- New control channel in the recorder (`src/win/control.c`): a per-user named pipe
  `\\.\pipe\WinFreezeTrace.Control.v1.<session>` (current user and SYSTEM only, local clients only) with the commands
  `STATUS`, `PAUSE`, `RESUME`, `CAPTURE`, `EXIT`. The Manager is a pure client of it.
- `ui.c` became `hostwin.c`: the hidden window that keeps the hotkey, suspend/resume and session-end handling.
- Removed `startup.c`; `--add-startup` / `--remove-startup` now fail with a pointer to the Manager. `--no-tray` and
  `--no-notify` are still accepted and ignored so that 0.5.x startup entries and tasks keep working.
- Left-click on the tray icon now opens the Manager window (it used to toggle monitoring); the toggle is in the menu and on the dashboard.

### Added

- WinFreezeTrace Manager: dashboard, report browser with `summary.txt` preview, settings for every recorder option,
  per-user Start with Windows (migrates a 0.5.x entry), single instance, `--background`, `--exit`.
- Installer: the Manager is an optional component, **selected by default** (`/MANAGER=0` for silent installs without it).
  Start Menu shortcut and the all-users startup entry point at the Manager when it is installed.
- The release workflow publishes the Manager for x64 and x86 and bundles it in the installer and the portable zip.

### Verification status

- Core tests: 119 checks pass under AddressSanitizer/UBSan. `control.c` and `hostwin.c` pass `-fsyntax-only` against stand-in headers.
- **Not yet built or run:** the Manager (C#/XAML), the changed Windows C files against the real SDK, and the NSIS script.
  The first CI run and a manual pass on Windows are required.

## 0.5.0

### Verification status

- Core: **120 assertions** in `tests/test_core.c` pass natively under AddressSanitizer/UBSan; the same suite passes (121) as a
  Windows PE under Wine.
- Windows layer: now **compiles without warnings with MinGW-w64 for x64 and x86** (0.4.0 did not build with MinGW at all, see
  below). Under Wine the app was exercised end to end: start-up entry, nested `--report-dir`, single instance, `--exit`,
  tray on/off, hotkey capture and report writing. Wine lacks the Event Log API, PDH counters and real wait chains, so
  those paths, the real tray rendering and the MSVC build still need a pass on genuine Windows.

### Added

- **Tray icon** (generated at run time, no resource files): click = monitoring on/off, right-click = menu with status,
  on/off, *Start with Windows*, *Open reports folder* and *Exit*. Icon shows on (green), off (grey) and writing a report
  (orange); a balloon announces a saved report and opens the folder when clicked. The icon is restored if Explorer
  restarts. While off there is no sampling, probing or capturing, and the history is discarded.
- **Start with Windows**: per-user entry in Settings > Apps > Startup (honours the Task Manager enable/disable switch).
  CLI: `--add-startup`, `--remove-startup` (they store the other options, with an absolute `--report-dir`).
  The installer gains an optional all-users entry.
- `--exit` (quit the running instance of this session), `--no-tray`, `--no-notify`.
- A second launch now makes the running instance show an "already running" balloon instead of exiting silently.
- `startup_apps.csv` in every report: Run/RunOnce keys and Startup folders, with their enabled/disabled state.
- Build fails at configure time if `CMakeLists.txt`, `wft_types.h` and `version.rc` disagree on the version.

### Fixed

- **MinGW build was broken**: `WCT_MAX_NODE_COUNT`, `WCTP_GETINFO_ALL_FLAGS`, `WctSocketIoType` and `WctSmbIoType` are not in
  MinGW's `wct.h`. Guarded fallbacks added; also a signed/unsigned PDH status comparison.
- Removed `src/main.c`: an outdated, unbuilt duplicate of `engine.c`.
- **Event ages were measured from the end of the capture**, not from the trigger. The minidump can take up to 15 s, so
  `events_digest.csv` and the "near the trigger" weighting were skewed by that amount. The trigger time is now taken first.
- `--report-dir` with missing parent folders made every capture fail; parents are now created (only the leaf is private).
- A manual capture waited for the next sampling tick (up to `--interval`, e.g. 60 s). The loop now wakes immediately.
- The hotkey/UI thread ran at below-normal priority, so it could be starved exactly when the system is under load.
- Sleep detection relied only on a power broadcast. A second clock that stops during sleep (`QueryUnbiasedInterruptTime`)
  now removes sleep time from the lateness, so a short sleep or Modern Standby can no longer be reported as a stall.
  A multi-week gap can no longer overflow the 32-bit lateness value.
- Process-snapshot window length was left as a raw timestamp on the failure path (bogus "window" in `summary.txt`).
- Module snapshot now retries on `ERROR_BAD_LENGTH`; `%p` replaced by explicit hex (it printed `0x` twice on some runtimes);
  VRAM size is also read when stored as 8-byte `REG_BINARY`.
- A WHEA event with an unknown level was classified as *uncorrected*; an empty timeline no longer indexes `s[-1]`.
- The process now declares itself DPI-aware (sharp menu, correct window geometry).
- Scripts: `README` claimed there was no `LICENSE`; `install_task.cmd` hard-coded the version and now asks a running
  instance to exit cleanly before killing it; `uninstall_task.cmd` removes the Startup entry.

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
