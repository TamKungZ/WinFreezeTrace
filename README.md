# WinFreezeTrace

Current version: **0.4.0**

WinFreezeTrace is a small Windows background recorder for investigating freezes and unresponsive foreground applications. It keeps a short in-memory timeline and writes a diagnostic bundle when it detects a trigger or when the user presses a hotkey.

The generated summary is heuristic. It points to evidence worth investigating, but it does not determine root cause with certainty and is not a replacement for ETW/WPR, a kernel dump, hardware diagnostics, or a debugger.

## Project status

This is an experimental troubleshooting tool. Report formats, trigger rules, and diagnostic thresholds may change. Test it on your own system before relying on it for long unattended sessions. See [CHANGELOG.md](CHANGELOG.md) for what changed in 0.4.0 and how it was verified.

No `LICENSE` file is currently included. Add an appropriate open-source license before publishing or accepting contributions; source availability alone does not grant reuse rights.

## How it works

Each sampling interval (one second by default) the process records:

- CPU user, privileged, DPC and interrupt time, **plus the worst single logical CPU** for load, DPC and interrupt time (an average over all cores hides one pegged core)
- interrupt rate, queued DPC rate, context switches, CPU performance % and frequency
- disk queue depth, throughput, IOPS and latency, **plus the worst single physical disk** (name, latency, queue)
- memory load, commit, paging, page faults, paged/nonpaged pool, system handle/thread/process counts
- the foreground window identity and a bounded responsiveness probe, including **how long the window took to answer** and *why* a probe failed (timeout, hung-window flag, blocked by UIPI, secure desktop)

Samples live in a heap-allocated RAM ring (default ~180 samples, well under 1 MiB). Counters that Windows cannot provide are stored as "unavailable" and shown as `n/a`/empty cells rather than as `0`. No report files are written until a capture is triggered.

## Performance characteristics

The steady-state path is deliberately smaller than the capture path:

- It does not continuously run ETW/WPR, create dumps, read Event Logs, enumerate every process, or enumerate drivers and devices.
- It collects system counters and probes the foreground window once per interval.
- The responsiveness probe may wait up to `--hang-timeout-ms` when the foreground window is not responding. That wait happens in WinFreezeTrace, not in the monitored application.

This design is intended to be usable while gaming, but the project does **not** guarantee zero FPS loss, zero latency impact, or compatibility with every game and anti-cheat system. Measure frametime and CPU use on the target PC before a competitive or latency-sensitive session.

A capture is intentionally heavier than steady-state monitoring (process/thread/module enumeration, wait chains, Event Log queries, driver version lookup, device inspection, possibly a minidump). `summary.txt` reports how long each capture step took. **Writing the minidump suspends the target process while it runs**, which can add a visible hitch to a game or app that is already struggling; `--no-dump` avoids this.

For lower overhead or lower data sensitivity:

```text
WinFreezeTrace.exe --interval 2000 --no-dump
```

## Trigger conditions

- **Scheduler/watchdog delay:** a sampling wake is at least 1500 ms late (default).
- **Foreground application timeout:** the foreground window fails the `WM_NULL` probe (250 ms default) for three consecutive samples.
- **Manual capture:** press **Ctrl+Alt+F12** (use `--no-hotkey` if another program owns it). A manual capture is never blocked by the automatic cooldown, only rate-limited to one per 3 s.

Automatic captures have a 60-second cooldown. Delays of 60 s or more are ignored (sleep/hibernate), and **stalls within 15 s after a suspend/resume notification are ignored** to avoid reporting wake-from-sleep as a freeze. Probes made while the UAC prompt or lock screen is showing, or against an elevated window the recorder cannot message, are not counted as hangs. These rules can still produce false positives and negatives; a late userspace watchdog does not by itself prove the kernel froze.

## Report contents

Reports are written to `%LOCALAPPDATA%\WinFreezeTrace\reports\YYYYMMDD_HHMMSS_reason\` unless `--report-dir` is supplied. Each incident folder is created readable only by the current user and SYSTEM. If two incidents get the same name a `_2`, `_3`… suffix is added.

| File | Contents |
|---|---|
| `summary.txt` | Trigger, foreground app, **ranked findings with evidence and next steps**, baseline-vs-lead-in metric table, top CPU/I-O processes, hottest threads, Event Log correlation, capture cost and warnings |
| `summary.json` | The same findings and metrics in machine-readable form |
| `timeline.csv` | Pre-trigger history (all metrics above, worst-CPU/disk names, probe status and latency) |
| `foreground.txt` | Foreground app: elevation, bitness, age, CPU time, memory, handles, I/O, window geometry, fullscreen/borderless, hung flag, user idle time |
| `processes.csv` | Every process: CPU %, **disk read/write B/s, page-fault rate**, memory, session, parent, handles, foreground flag |
| `threads.csv` | Foreground-process threads: **name, per-thread CPU % in the window**, priorities, UI-thread flag |
| `waitchains.txt` | Wait Chain Traversal; status names, wait times, and **the name of the other process** a thread is blocked on |
| `modules.csv` | DLLs loaded in the foreground process |
| `target.dmp` | Minidump of the foreground process (see safeguards below) |
| `events_digest.csv` | **Classified Event Log records** with provider, ID, level, message and *seconds before the trigger* |
| `events_system.xml`, `events_application.xml`, `events_performance.xml` | Raw recent events (omitted with `--redact`) |
| `system_info.txt` | Windows build/revision, system/board/BIOS, CPU, memory, uptime, power source |
| `power.txt` | Active power plan, per-CPU max/current/limit MHz, thermal zones and passive limit |
| `drives.csv` | Volume capacity/free space (network and optical drives are skipped, since they can hang) |
| `storage_devices.csv` | Physical disks: bus type, **HDD vs SSD**, firmware |
| `loaded_drivers.csv` | Kernel drivers with **vendor, file version, file date, third-party flag** |
| `displays.csv`, `display_drivers.csv` | Display adapters and **GPU driver version/date/VRAM** |

Files may be empty or absent when an API, event channel, process or device cannot be accessed.

## What the analysis does

The summary compares the **lead-in window** (the last `--lead-window` seconds, default 30) with the **baseline** before it, so an unrelated spike minutes earlier is not reported as a finding. Each finding has a confidence (`HIGH`/`MED`/`LOW`/`INFO`), the numbers behind it (value, how many seconds before the trigger it peaked, baseline median) and a concrete next step. Current rules cover:

- wait-chain cycles and a UI thread blocked behind another thread or process
- Event Log events **classified by provider and ID** and weighted by distance from the trigger: uncorrected vs corrected WHEA, storage, file-system, GPU driver / TDR, `LiveKernelEvent`, application hang/crash, firmware CPU throttling, bugchecks, unexpected shutdowns, sleep/resume
- memory/commit pressure, hard paging, DPC/interrupt time (total and single-CPU), CPU saturation, disk queue and latency (total and worst disk), CPU running far below base performance, thermal limits, nearly full volumes, nonpaged pool growth
- foreground-only hangs while the watchdog keeps scheduling normally, and slow UI message handling

Thresholds are generic. A matching rule is evidence, not proof; a report with no findings does not show the system is healthy.

## Safeguards and hardening

- **Minidump:** least-privilege handle; never dumps `lsass.exe`, `csrss.exe`, `winlogon.exe`, `wininit.exe`, `services.exe`, `smss.exe`; skips itself; is aborted (and the partial file deleted) after `--dump-timeout-ms`; skipped when free space is below `--min-free-mb`.
- **Disk use:** only the newest `--max-reports` (default 50) incident folders are kept. Pruning only ever deletes folders named like an incident, never follows links, and refuses folders containing anything but known report file types.
- **Privacy:** report folders have a protected owner-only ACL. `--redact` masks user/computer names, serial numbers, window titles and user-profile paths, and skips raw Event XML. It does **not** sanitise `target.dmp`; combine with `--no-dump` if that matters.
- **Output safety:** CSV fields are neutralised against spreadsheet formula injection (a window title such as `=HYPERLINK(...)` is written as text), control characters are removed, and text is converted to UTF-8 correctly including surrogate pairs.
- **Process:** DLLs are loaded from System32 only; heap corruption terminates the process; no "insert disk" dialogs; command-line values are strictly validated; a small size-capped log is kept at `%LOCALAPPDATA%\WinFreezeTrace\winfreezetrace.log`; the recorder shuts down cleanly on logoff/shutdown.

## Known limitations

- A complete hard lock may prevent WinFreezeTrace from running and writing a report.
- Counters sampled once per second can miss short spikes.
- The watchdog measures scheduling delay in this userspace process; CPU contention, power transitions and debugging can also delay it.
- `WM_NULL` is a basic message-loop check; it does not describe every kind of application or GPU hang.
- Process CPU/I-O rates are measured in the ~0.4 s **after** the trigger. For what happened before it, rely on `timeline.csv`.
- Per-disk/per-CPU instances are enumerated when the program starts; a disk attached later is not tracked until restart.
- The minidump covers the foreground process, not the kernel or every process involved in a system-wide stall.
- A normal user cannot inspect every elevated, protected or anti-cheat-protected process and driver.
- Thermal-zone and CPU-frequency data depend on what the firmware/driver exposes; many desktops expose none.
- Minidump creation, process inspection and wait-chain walking are behaviours that some security products and anti-cheat systems flag. Test before using with protected games.
- There is no automated performance, frametime or anti-cheat testing.

For recurring system-wide freezes without a useful signature, a bounded kernel ETW/WPR trace, kernel dump configuration, vendor diagnostics or stress testing may be needed.

## Privacy and security

Reports can contain the Windows user and computer names, window titles, executable paths, process/module lists, storage serials, driver paths, Event Log text and (in `target.dmp`) process memory. Review and redact before sharing, and treat minidumps as sensitive. Run as a normal user. WinFreezeTrace does not upload anything; all processing is local.

## Source layout

```text
src/main.c            start-up, sampling loop, shutdown
src/core/             platform-independent logic (no <windows.h>); unit-tested
  strutil.*           safe formatting, UTF-8, CSV/JSON output, strict parsing, path redaction
  config.*            command line parsing and validation
  ring.*              heap ring buffer of samples
  stats.*             window statistics (median/p95/peak and when it peaked)
  trigger.*           when to capture (cooldown, resume grace, sleep gaps, hang streak)
  eventclass.*        classify Event Log records into diagnostic categories
  findings.*          ranked findings engine and text/JSON writers
  retention.*         which old report folders may be deleted
src/win/              Windows API layer
  platform.*          hardening, time, private folders, logging
  metrics.*           PDH counters (single and per-instance) and memory info
  foreground.*        foreground window probe and details
  ui.*                hidden window: hotkey, suspend/resume, session end
  procsnap.*          two-point process snapshot
  threadinfo.*        threads, wait chains, modules
  dump.*              bounded, deny-listed minidump
  events.*            Event Log export and classified digest
  sysinfo.*           system, drives, drivers, storage, displays, power
  report.*            capture orchestration, summary writers, retention
tests/test_core.c     unit tests for src/core
register_task.ps1     scheduled-task registration used by install_task.cmd
```

## Build

### Visual Studio / Developer Command Prompt

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The executable is written to `build\Release\WinFreezeTrace.exe`. `ctest` runs the core unit tests.

### MinGW-w64

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build
```

(`-municode` is added automatically because the program uses `wWinMain`.)

### Tests on any platform

```sh
./tests/run.sh        # builds src/core + tests with sanitizers and runs them
```

The program links only to Windows system libraries (user32, psapi, pdh, wevtapi, advapi32, dbghelp, shell32, powrprof, version).

## Run and install

```bat
WinFreezeTrace.exe --help
WinFreezeTrace.exe
```

`--help` and `--version` print to the console that launched the program. To install for the current user, place `WinFreezeTrace.exe` next to `install_task.cmd` (or build Release), then run:

```bat
install_task.cmd
install_task.cmd --redact --max-reports 20     (options are passed to the task)
```

The installer stops this user's existing instance, copies the executable to `%LOCALAPPDATA%\WinFreezeTrace\bin`, and registers a logon task through `register_task.ps1` at limited privilege **with no execution time limit, allowed on battery, restart on failure** (tasks created by plain `schtasks /Create` would be killed after 3 days and would not run on battery). Administrator access is not normally required. `register_task.ps1` is run with `-ExecutionPolicy Bypass` for that single process only; read it first if you prefer.

```bat
uninstall_task.cmd            remove task and executable, keep reports
uninstall_task.cmd --purge    also delete reports and logs (cannot be undone)
```

Only one instance runs per user session.

## Options

```text
--interval <ms>          Sampling interval            (250..60000, default 1000)
--history <s>            Seconds of RAM history       (30..600, default 180)
--stall-ms <ms>          Watchdog lateness trigger    (500..30000, default 1500)
--hang-timeout-ms <ms>   Foreground WM_NULL timeout   (50..5000, default 250)
--hang-samples <n>       Failed probes in a row       (2..20, default 3)
--cooldown <s>           Gap between auto captures    (5..86400, default 60)
--lead-window <s>        Lead-in window for analysis  (5..300, default 30)
--report-dir <path>      Report root
--max-reports <n>        Keep newest n incident folders (0 = unlimited, default 50)
--min-free-mb <n>        Skip minidumps below this free space (default 1024, 0 = off)
--no-dump                Never write target.dmp
--dump-timeout-ms <ms>   Abort a minidump after this long (1000..120000, default 15000)
--redact                 Mask names, serials, titles; skip raw event XML
--no-hotkey              Do not register Ctrl+Alt+F12
--help, --version
```

Options also accept `--name=value`. Out-of-range numbers are clamped (a note is written to the log and to `summary.txt`); unknown options and non-numeric values are rejected with exit code 64. The ring is capped at 600 samples, so with intervals below 300 ms the effective history is shorter than `--history`.

## Reading a report

Start with `summary.txt`: the ranked findings say what to look at and why; then verify against the underlying files.

- **Wait cycle / UI thread blocked:** `waitchains.txt`, then `target.dmp` in WinDbg (`~*k`) with symbols.
- **Which process was busy:** top CPU/I-O in the summary and `processes.csv`; remember it is measured after the trigger.
- **Which thread was spinning:** `threads.csv`, `cpu_one_core_pct_in_window`, UI-thread flag.
- **GPU hang:** `events_digest.csv` (Display / `livekernel_event`), `display_drivers.csv` driver version, `power.txt` thermals.
- **Storage:** the worst-disk name/latency in the summary, `storage_devices.csv` (HDD/SSD, firmware), storage events.
- **Driver latency:** DPC/interrupt findings, then `loaded_drivers.csv` filtered to `third_party=yes`, then a WPR/LatencyMon trace.
- **Sleep/resume noise:** an `INFO` sleep/resume finding means a stall near a power transition is probably not a real freeze.

Do not diagnose from `summary.txt` alone.
