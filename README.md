# WinFreezeTrace v2 — lightweight background freeze/hang recorder

WinFreezeTrace is designed to answer five questions after a Windows freeze/hang:

1. **When?** Exact local trigger time plus a rolling pre-trigger timeline.
2. **Where?** Foreground process/window, its modules, wait chains, and a small minidump.
3. **What happened?** CPU (user/kernel), DPC/interrupt rate, context switches, memory/commit/paging, disk queue/latency/IOPS, processor queue, app responsiveness, and a detailed process snapshot.
4. **How did it develop?** The last 180 seconds are held in RAM and flushed only when a trigger occurs.
5. **Why is it likely happening?** A rule-based summary correlates deadlocks, WHEA, storage/GPU events, memory pressure, DPC/interrupt spikes, CPU starvation, and application-only hangs.

## Why this stays light

Normal background operation does **not** continuously write logs to disk and does **not** continuously run ETW/WPR, enumerate every process, walk stacks, or query Event Log.

The steady-state loop wakes once per second and collects a small set of PDH/system counters plus a foreground-window responsiveness probe. Data is stored in a fixed RAM ring buffer. Heavy work happens only after a trigger.

Default RAM history is tiny (180 samples). CPU use should normally be near idle between one-second wakeups.

## Trigger conditions

- **Whole-system/scheduler stall:** the one-second watchdog wake is late by at least 1500 ms.
- **Foreground app hang:** the foreground window fails a 250 ms `WM_NULL` responsiveness probe for 3 consecutive samples.
- **Manual capture:** press **Ctrl+Alt+F12** at any time.

A 60-second cooldown prevents report storms.

Long delays over 60 seconds are ignored as automatic scheduler-stall triggers to avoid treating sleep/hibernate resume as a freeze. You can still press Ctrl+Alt+F12 after resume if you want a report.

## What each incident contains

- `summary.txt` — trigger, evidence, and likely-cause analysis with confidence labels.
- `timeline.csv` — rolling pre-trigger metrics from RAM.
- `processes.csv` — trigger-time process snapshot sorted by short-window CPU usage; also threads, working/peak/private memory, page faults, I/O operations/bytes, handles, start time, and path.
- `waitchains.txt` — Wait Chain Traversal for every thread of the foreground process; highlights circular waits/deadlocks.
- `threads.csv` — target thread IDs, priorities, and accumulated kernel/user CPU time.
- `modules.csv` — loaded DLL/module map for the foreground process.
- `target.dmp` — small minidump focused on threads/process state. Open in WinDbg for exact stacks/symbols. Created only on trigger.
- `events_system.xml` — last 30 minutes of System Critical/Error/Warning events.
- `events_application.xml` — last 30 minutes of Application Critical/Error/Warning events.
- `events_performance.xml` — recent Windows Diagnostics-Performance events when the channel is accessible.
- `drives.csv` — volume label, filesystem, cluster size, capacity, and free-space snapshot.
- `system_info.txt` — Windows build, computer/BIOS model, CPU, memory/commit, uptime, and power state.
- `loaded_drivers.csv` — kernel drivers loaded at trigger time.
- `storage_devices.csv` — physical disk model, firmware, serial, bus type, and command-queueing support.
- `displays.csv` — display adapters, hardware IDs, registry keys, and active-state flags.

Reports are stored by default under:

`%LOCALAPPDATA%\WinFreezeTrace\reports\YYYYMMDD_HHMMSS_reason\`

## What the diagnosis looks for

- **Deadlock / circular wait:** Wait Chain Traversal reports a cycle.
- **Hardware / PCIe / CPU / RAM:** recent WHEA events.
- **Storage stall:** storport/storahci/stornvme/disk events and/or high disk queue without CPU saturation.
- **GPU/driver reset:** display-driver events near the incident.
- **Memory pressure:** commit or memory load near exhaustion.
- **Hard paging:** sustained pages read from disk, often caused by memory pressure or a slow pagefile device.
- **Storage latency:** response time over 100 ms, together with queue depth, throughput, and IOPS.
- **Driver interrupt latency:** high DPC or interrupt CPU time.
- **CPU starvation:** CPU near 100% plus a processor queue longer than the logical CPU count.
- **App-only hang:** foreground app stops responding while the watchdog remains schedulable.

The summary intentionally says “likely” rather than pretending a heuristic proves root cause.

## Build

### Visual Studio / Developer Command Prompt

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Result is normally:

`build\Release\WinFreezeTrace.exe`

### MinGW-w64

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

No third-party runtime libraries are required beyond normal Windows system DLLs.

## Run in the background

The target is a `WIN32` GUI executable, so it does not leave a console window open.

Simply run:

```bat
WinFreezeTrace.exe --background
```

For automatic start at user logon, put `WinFreezeTrace.exe` next to `install_task.cmd` and run the script once. It creates a **LIMITED / normal-user** scheduled task, so it does not unnecessarily run elevated.

Use `uninstall_task.cmd` to remove it.

## Useful options

```text
--interval 1000          Sampling interval in ms (minimum 250)
--history 180            Seconds kept in the in-memory ring (30..600)
--stall-ms 1500          Watchdog lateness that triggers a system-stall capture
--hang-timeout-ms 250    Window responsiveness timeout per sample
--hang-samples 3         Consecutive failed probes before app-hang capture
--cooldown 60            Minimum seconds between automatic reports
--no-dump                Disable target.dmp if you want the smallest incident footprint
--report-dir "D:\path"   Override report directory
```

For the lightest default background behavior, keep the 1000 ms interval. Lowering it below 500 ms provides finer timing but creates more wakeups.

## Reading a report quickly

Start with `summary.txt`, then:

- If it says **wait-chain cycle**, inspect `waitchains.txt`; open `target.dmp` in WinDbg for call stacks.
- If it flags **WHEA/storage/display**, compare exact event timestamps in the XML files to the freeze time.
- If it flags **CPU starvation**, inspect the first rows of `processes.csv` and the last 10–20 rows of `timeline.csv`.
- If it flags **memory pressure**, inspect commit/memory columns in `timeline.csv`.
- If no strong signature exists but a scheduler stall was real, a kernel ETW/WPR trace is the next escalation. WinFreezeTrace intentionally avoids keeping kernel ETW on all the time because that would defeat the “light background monitor” goal.

## Privilege note

Running as a normal user is recommended for low-risk background use. A normal user may be unable to read another elevated/protected process, so that process's minidump/modules can be missing. The system metrics and accessible Event Log data still get written. If a specific elevated application is the recurring target, you can run WinFreezeTrace elevated for that troubleshooting session only.
