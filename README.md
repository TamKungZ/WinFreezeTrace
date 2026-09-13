# WinFreezeTrace

WinFreezeTrace is a small Windows background recorder for investigating freezes and unresponsive foreground applications. It keeps a short in-memory timeline and writes a diagnostic bundle when it detects a trigger or when the user presses a hotkey.

The generated summary is heuristic. It can point to evidence worth investigating, but it does not determine root cause with certainty and is not a replacement for ETW/WPR, a kernel dump, hardware diagnostics, or a debugger.

## Project status

This is an experimental troubleshooting tool. Report formats, trigger rules, and diagnostic thresholds may change. Test it on your own system before relying on it for long unattended sessions.

No `LICENSE` file is currently included. Add an appropriate open-source license before publishing or accepting contributions; source availability alone does not grant reuse rights.

## How it works

During normal operation, the process wakes at the configured sampling interval (one second by default) and records:

- CPU user, privileged, DPC, and interrupt activity
- interrupt rate, queued DPC rate, and context switches
- disk queue depth, throughput, IOPS, and average latency
- memory load, commit usage, paging, page faults, and nonpaged pool size
- system process, thread, and handle counts
- foreground window identity and the result of a bounded responsiveness probe

Samples are stored in a fixed-size RAM ring. The default configuration retains approximately 180 samples. The program does not intentionally create or append report files until a capture is triggered.

## Performance characteristics

The steady-state path is deliberately smaller than the capture path:

- It does not continuously run ETW/WPR, create dumps, read Event Logs, enumerate every process, or enumerate drivers and devices.
- It collects system counters and probes the foreground window once per sampling interval.
- The default ring buffer consumes less than 1 MiB for sample data. Normal process working set is higher because Windows runtime and loaded system libraries are also included.
- The responsiveness probe may wait for up to `--hang-timeout-ms` when the foreground window is not responding. This wait occurs in WinFreezeTrace, not in the monitored application.

This design is intended to be usable while gaming, but the project does **not** guarantee zero FPS loss, zero latency impact, or compatibility with every game and anti-cheat system. Performance depends on the machine, Windows build, counter providers, sampling interval, and foreground application. Measure frametime and CPU use on the target PC before a competitive or latency-sensitive session.

A capture is intentionally heavier than steady-state monitoring. It enumerates processes and target threads/modules, queries wait chains and Event Logs, inspects devices, and may write a minidump. This can briefly consume CPU and storage bandwidth. A false trigger during gameplay may therefore add to an existing hitch.

For lower overhead or lower data sensitivity:

```text
WinFreezeTrace.exe --interval 2000 --no-dump --background
```

A longer interval reduces wakeups but lowers timeline resolution and can make short stalls harder to characterize. `--no-dump` reduces capture-time work and report size, but removes the most useful artifact for inspecting application thread stacks.

## Trigger conditions

- **Scheduler/watchdog delay:** a sampling wake is at least 1500 ms late by default.
- **Foreground application timeout:** the foreground window fails a 250 ms `WM_NULL` probe for three consecutive samples by default.
- **Manual capture:** press **Ctrl+Alt+F12**.

Automatic captures have a 60-second cooldown. Delays of 60 seconds or more are ignored as automatic scheduler-stall triggers to reduce captures caused by sleep or hibernation. These rules can produce false positives and false negatives; a late userspace watchdog does not by itself prove that the Windows kernel froze.

## Report contents

Reports are stored under the following directory unless `--report-dir` is supplied:

```text
%LOCALAPPDATA%\WinFreezeTrace\reports\YYYYMMDD_HHMMSS_reason\
```

An incident directory can contain:

- `summary.txt`: trigger details and rule-based findings with confidence labels
- `timeline.csv`: the in-memory pre-trigger history
- `processes.csv`: a trigger-time process snapshot, including short-window CPU use, memory, faults, I/O totals, handles, and thread counts
- `waitchains.txt`: Wait Chain Traversal results for threads in the foreground process
- `threads.csv`: foreground-process thread priorities and accumulated CPU times
- `modules.csv`: loaded modules visible in the foreground process
- `target.dmp`: a small foreground-process minidump, unless disabled or inaccessible
- `events_system.xml`: up to 160 recent warning, error, and critical System events from a 30-minute window
- `events_application.xml`: up to 160 recent warning, error, and critical Application events from a 30-minute window
- `events_performance.xml`: accessible Diagnostics-Performance events from a one-hour window
- `system_info.txt`: Windows build, system/BIOS model, CPU, memory, uptime, and power state
- `drives.csv`: mounted-volume capacity, free space, filesystem, and cluster size
- `storage_devices.csv`: physical-storage identity and capability fields exposed by Windows
- `loaded_drivers.csv`: loaded kernel modules visible to the current account
- `displays.csv`: display adapters and device identifiers reported by Windows

Files may be empty or absent when an API, event channel, process, or device cannot be accessed.

## What the summary checks

Current rules look for correlations such as:

- a wait-chain cycle in the foreground process
- recent WHEA, storage, display, NTFS, application-hang, or power events
- high memory/commit use or heavy hard paging
- elevated disk queue depth or average storage latency
- high DPC or interrupt CPU time
- CPU saturation together with a long processor queue
- a foreground-only timeout while the watchdog continues to schedule normally

Thresholds are generic and are not calibrated for every workload or device. A matching rule is evidence, not proof. Conversely, a report with no matching rule does not establish that the system is healthy.

## Known limitations

- A complete hard lock may prevent WinFreezeTrace from running and writing a report.
- Metrics sampled once per second can miss short spikes.
- The watchdog measures scheduling delay in this userspace process; CPU contention, power transitions, and debugging can also delay it.
- `WM_NULL` is only a basic message-loop responsiveness check. It does not describe every type of application or GPU hang.
- Aggregate disk metrics do not identify which physical device caused a latency spike.
- The minidump covers the foreground process, not the kernel or every process involved in a system-wide stall.
- Event Log proximity and provider-name matching can create coincidental findings.
- Normal-user execution cannot inspect every elevated, protected, or anti-cheat-protected process and driver.
- Device model, firmware, serial, driver path, and display fields depend on what Windows exposes to the current account.
- The current test suite does not include automated performance, game frametime, or anti-cheat compatibility testing.

For recurring system-wide freezes without a useful signature, a bounded kernel ETW/WPR trace, Windows kernel dump configuration, vendor hardware diagnostics, or targeted stress testing may be needed.

## Privacy and security

Incident reports can contain sensitive data, including the Windows user and computer names, foreground window titles, executable paths, process/module lists, storage serial numbers, driver paths, Event Log XML, and process memory in `target.dmp`.

Review and redact reports before posting them publicly. Treat minidumps as potentially containing private application data, credentials, document fragments, or other process memory. Run the program as a normal user unless elevated access is specifically needed for troubleshooting.

WinFreezeTrace does not upload reports. All report generation in this repository is local, but users should review source and release provenance before running third-party binaries.

## Build

### Visual Studio / Developer Command Prompt

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is normally written to:

```text
build\Release\WinFreezeTrace.exe
```

### MinGW-w64

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

The program links only to Windows system libraries. Successful compilation does not imply that every capture API is available or permitted on every supported Windows configuration.

## Run

WinFreezeTrace is built as a Windows GUI executable and does not keep a console window open.

```bat
WinFreezeTrace.exe --background
```

To start it at user logon, place `WinFreezeTrace.exe` next to `install_task.cmd` and run the script. The task uses the normal user's limited privilege level. Run `uninstall_task.cmd` to remove the task.

Only one instance is allowed per user session.

## Options

```text
--interval 1000          Sampling interval in ms (minimum 250)
--history 180            Seconds requested for the RAM history (30..600)
--stall-ms 1500          Watchdog lateness that triggers a capture
--hang-timeout-ms 250    Foreground-window probe timeout
--hang-samples 3         Consecutive failed probes required for a capture
--cooldown 60            Minimum seconds between automatic reports
--no-dump                Do not create target.dmp
--report-dir "D:\path"   Override the report directory
```

The ring is capped at 600 samples. With intervals longer than one second, the requested history duration contains fewer samples. With intervals below one second, the 600-sample cap can shorten the effective history duration.

## Reading a report

Start with `summary.txt`, then verify each finding against the underlying files:

- For a wait-chain cycle, inspect `waitchains.txt` and, if available, `target.dmp` in WinDbg with appropriate symbols.
- For WHEA, storage, display, NTFS, or power findings, compare exact timestamps and event details in the XML files.
- For CPU saturation, inspect the final timeline rows and the first rows of `processes.csv`.
- For memory pressure, inspect commit, available memory, pages-input, and page-reads columns.
- For storage stalls, inspect latency, queue, IOPS, throughput, paging, and related System events together.

Do not diagnose from `summary.txt` alone.
