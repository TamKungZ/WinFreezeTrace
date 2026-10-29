# Changelog

## Unreleased

### Fixed

- **Stalls on the system disk were caught late or not at all.** The 0.2 triggers (slow disk, deep disk queue) had been dropped, so only a foreground app failing three
  probes in a row could fire. In a real report (five 12-17 s stalls of drive C: within 3 minutes) the app-hang trigger fired 12-13 s after a stall began, and
  never for the worst one (10 s latency, queue 212), where the foreground app kept answering its message queue. See Added.
- **Time spent blocked inside the sampling step was invisible.** Lateness was measured around the wait only, so a long `PdhCollectQueryData`, process snapshot or probe
  did not count. It is now added to the lateness of that sample (minus any time the machine slept), and a log line says how it split between counters and probe.
- **The Store (MSIX) package crashed at launch** (Partner Center certification 10.1.2.10: Application Error 1000, exception 0xc000027b in `Microsoft.UI.Xaml.dll`, the Manager exits within two seconds).
  The package put the Manager in a `Manager\` subfolder and had no `resources.pri`. WinUI 3 resolves `ms-appx:///` (compiled XAML) and `resources.pri` against the package root, so the XAML could not be loaded.
  `pack-msix.ps1` now places the Manager at the package root and installs its resource index as `resources.pri`; the manifest points to the new path. The NSIS installer and the zip are unchanged.
  The Manager now logs a failure in `App` start-up (also to `%TEMP%\WinFreezeTrace.Manager.startup.log`) instead of leaving only the event-log entry. **Not verified in a package**: test with `Add-AppxPackage -Register <stage>\AppxManifest.xml` before submitting.
- `system_info.txt` said "Windows 10 Pro" on Windows 11 (the registry keeps the old name). Builds >= 22000 now show "Windows 11".

### Added

- **Storage stall trigger** (`storage_stall`): worst-disk latency >= 500 ms, or worst-disk queue >= 10 for two samples. Options `--disk-latency-ms` and `--disk-queue`
  (`0` = off). Ignored during the resume grace period and the cooldown. A storage-stall capture does not write `target.dmp`: the foreground app is not the cause, the dump suspends it, and it would be written to the slow disk. `summary.txt` shows the worst disk at the trigger; the Manager labels these reports "Storage stall".

### Verification status

- Core tests: 139 checks pass under AddressSanitizer/UBSan (20 new: storage trigger, cooldown, resume grace, queue streak, unavailable counters, `0` = off, config).
- The Windows layer and the DLL compile and link without warnings with MinGW-w64 (x64). **Not run on Windows**: the new lateness and the trigger have not been seen firing live.
  Replaying a real 180 s timeline through the trigger logic fires at the start of the stalls, 12-13 s before the app-hang trigger. The Manager change is a single label and was not compiled.
- Defaults are the 0.2 values and have not been tuned. A busy HDD can exceed 500 ms legitimately; raise `--disk-latency-ms` if reports are noisy.

## 0.8.0

### Added

- **Updating by running the new installer over the old one.** The installer now treats an existing install as an update instead of a fresh install:
  - The old Manager and recorder are asked to quit, then the installer waits until their files are free (up to about 10 s). If a file is still locked
    (for example an instance in another user's session, which `--exit` cannot reach) they are ended forcibly. This replaces the fixed 1.5 s pause of 0.7.0.
  - The `Manager` folder is replaced instead of merged, so files of an older Manager no longer pile up.
  - The copy of the recorder that `install_task.cmd` keeps in `%LOCALAPPDATA%\WinFreezeTrace\bin` is updated for every user profile that has one, so the logon
    task runs the new version too. The uninstaller removes these copies (reports, settings and logs in `%LOCALAPPDATA%\WinFreezeTrace` are still kept).
  - The choices of the previous install (Manager, PATH, Start with Windows, Start Menu shortcuts) are pre-ticked, and a component that is unticked is removed,
    so a re-install always matches the checkboxes. The recorder-only / Manager start-up entry and Start Menu shortcut are swapped when the Manager is added or removed.
- **Downgrade protection.** Installing an older version over a newer one asks first; silent installs refuse (exit code 5) unless `/ALLOWDOWNGRADE` is given.
  The installed version is stored as `VersionNum` (installs from before 0.8.0 are compared by their `DisplayVersion`).
- **Switching between the x86 and x64 installers** removes the other architecture first (asks, or does it silently with `/S`) instead of leaving two installs side by side.
  Reports and settings are kept; the other architecture's component choices are not carried over.
- **New silent-install switches:** `/PATH=0|1`, `/STARTUP=0|1`, `/SHORTCUTS=0|1` (and `/MANAGER=0|1`, which now also accepts `1`). Without a switch the choice of the previous install applies.
- README section "Installing and updating"; `PRIVACY.txt` now also mentions the log files and the removal of the `bin` copy.

### Changed

- **New application icon set.** A bright blue window with a title bar, a snowflake and a red record badge replaces the v7 icon. The three SVG sources (`assets/winfreezetrace.svg`,
  `assets/setup.svg`, `assets/uninstall.svg`) were replaced and everything built from them was regenerated, each size rendered from the SVG at its own size (16, 20, 24, 32, 40, 48, 64, 128, 256 px):
  `assets/winfreezetrace.ico` (shortcuts, Add/Remove Programs), the Manager's `app.ico` (executable and window icon) and the 256 px `logo.png` (title bar, About page),
  and `assets/setup.ico` / `assets/uninstall.ico` (green download-arrow and red trash-can badges for `Setup.exe` and `Uninstall.exe`). The four tray icons are unchanged: they only share the snowflake.
- The publisher name is now written as **TamKungZ_** (as in `LICENSE`) in Settings > Apps, the Manager's assembly metadata (`Company`), `TERMS.txt` and `PRIVACY.txt`. The GitHub links keep the repository address `TamKungZ/WinFreezeTrace`.

### Fixed

- **The 64-bit installer ignored the folder of an existing install.** `InstallDirRegKey` is read before the installer picks the 64-bit registry view, so an update of an
  install in a custom folder went to the default folder (a second copy), and an x86 install could be picked up as the folder of the x64 one. The folder is now read from the right view.

### Verification status

- The installer was compiled with makensis 3.09 (x64 and x86, with and without the Manager) and the following were run under Wine 9 with **placeholder program files**, not the real
  recorder and Manager: install, silent update of an older build (stale Manager files removed, choices kept), downgrade refusal and `/ALLOWDOWNGRADE`, components switched off by
  the switches, update into a custom folder, per-user copies updated and removed, x64 -> x86 and x86 -> x64 switching, update while the old exe was running and locked, and uninstall
  (no files, shortcuts or registry keys left; the user's other Run entries and data were untouched).
- **Not exercised:** a real Windows machine, the PATH step (Wine has no PowerShell), the interactive pages (the dialogs for downgrade and architecture switch), and a real update from the released 0.7.0 installer.
- The new icons were rendered, read back from the generated `.ico` files at 256, 48, 32 and 16 px, and checked inside the built installer: the icon in `Setup.exe` is `setup.ico` and the icon in
  the generated `Uninstall.exe` is `uninstall.ico`. They have **not** been seen on a real Windows desktop, tray or Start Menu, nor in the compiled Manager.
- Version bumped to 0.8.0 (`CMakeLists.txt`, `src/core/wft_types.h`, `src/version.rc`, Manager `.csproj`, `TERMS.txt`, `PRIVACY.txt`). The recorder and the Manager code are otherwise unchanged from 0.7.0.

## 0.7.0

### Changed

- **New application icon** (`WinFreezeTrace-icon-v7`): a snowflake on a blue window with a status badge. Replaces `assets/winfreezetrace.svg`,
  `assets/winfreezetrace.ico` (installer, shortcuts, Add/Remove Programs), the Manager's `app.ico` and the 256 px `logo.png` shown on the About page.
- The three tray icons (`tray_on.ico`, `tray_off.ico`, `tray_busy.ico`) were redrawn from the same artwork. The badge in the corner keeps the
  old meaning: green check = monitoring, grey pause = off/paused, orange ring = busy/capturing.
- **Tray icons redrawn in the Windows (Fluent) style** so they sit naturally among other notification-area icons: a flat, bold snowflake (the window frame was too detailed at 16 px)
  with the usual Windows status badges: green check = monitoring on, grey pause = monitoring off, orange ring = saving a report, red cross = recorder not running.
  "Recorder not running" has its own icon (`tray_stopped.ico`) instead of sharing the pause icon. The Dashboard and the navigation pane use the same colours.
- All icon files contain 16, 20, 24, 32, 40, 48, 64, 128 and 256 px images, each rendered from the SVG at its own size.
- **Separate icons for the installer and the uninstaller.** `Setup.exe` uses `assets/setup.ico` (green badge with a download arrow) and 
  `Uninstall.exe` uses `assets/uninstall.ico` (red badge with a trash can). Both keep the window + snowflake artwork of the application icon, so the two 
  programs can be told apart in Explorer, the taskbar and the Start Menu shortcut. Set through `MUI_ICON` / `MUI_UNICON` in `installer/nsis/WinFreezeTrace.nsi`;
  sources are `assets/setup.svg` / `assets/uninstall.svg`. The Add/Remove Programs entry keeps the application icon.
- **Terms of Use and Privacy Notice.** New `TERMS.txt` and `PRIVACY.txt` (what a report contains, memory-dump warning, redaction limits, where data is stored, no network use). The installer shows them as two extra pages after the license: Terms (must be accepted) and Privacy (Next only). Both are installed next to the program, removed by the uninstaller and included in the portable zip.
- **Installer/uninstaller clean-up pass.** The uninstaller now also removes the Manager's per-user "Start with Windows" entry (and the matching Startup apps 
  on/off flag) for every signed-in user instead of only the account that elevated it, removes the machine-wide Startup apps flag, waits for the Manager and 
  recorder to quit before deleting files, and releases its working directory so the install folder itself is removed. The installer waits for the old 
  processes on upgrade and records the installed size (`EstimatedSize`) for Settings > Apps.
- **Manager UI/UX pass** (Windows 11 look and clearer wording):
  - Title bar merged into the window, DPI-aware start size that always fits the screen, window centred on first show.
  - Window material is selectable in Settings: **Acrylic** (blurred, see-through; the new default), Mica, Mica Alt or Solid. It falls back
    automatically when the OS does not support the material.
  - The recorder state (on / off / saving / not running) is shown in the navigation pane on every page.
  - Dashboard: one clear status with one primary action; "Stop the recorder" moved into a **More** menu and asks for confirmation;
    warnings when the recorder program is missing or reports failed to save; saved/failed/hotkey tiles; a helpful empty state.
  - Reports: command bar with icons, F5 refresh and Del delete, readable trigger names ("System stall", "App not responding",
    "Manual capture") and "5 min ago" times, empty state, preview title.
  - Settings: grouped into Appearance, Start-up and tray, When to capture a report, What a report contains and Locations; every option has a plain-language
    description; a bar appears at the bottom as soon as a recorder option is changed (Save and restart recorder / Save only / Discard);
    unsaved edits survive switching pages; the memory-dump options grey out when the dump is off; a warning shows when redaction is on but the dump is not;
    the report folder and the recorder program are validated before saving; Browse button for WinFreezeTrace.exe.
  - Content cards now share one width instead of sizing to their text.
  - A one-time notification explains that closing the window leaves the Manager running in the tray.
- Version bumped to 0.7.0 (`CMakeLists.txt`, `src/core/wft_types.h`, `src/version.rc`, Manager `.csproj`).

### Verification status

- Icons were rendered and inspected at 256 px and 16 px. They have **not** yet been seen on a real Windows tray, in the installer or in the
  Manager's About page.
- The Manager UI changes were written without a Windows toolchain: every XAML file is well-formed, every `x:Name` and event handler used by the code-behind exists,
  and all C# files parse. **The Manager has not been compiled or run**; build it and click through every page (also with Windows 10, with
  "Transparency effects" turned off, and at 125 %/200 % scaling) before releasing. The installer was not rebuilt.

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
