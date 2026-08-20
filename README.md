# WinFreezeTrace

Native Windows C black-box recorder for intermittent freezes / storage stalls.

Version: **0.2.0**

## New in 0.2.0: background Windows service

WinFreezeTrace can now install itself as a normal Windows Service. The service:

- starts automatically at Windows boot
- does not require the user to log in
- has no console window while monitoring
- is controlled entirely from CMD / PowerShell
- restarts automatically after unexpected service failure
- stores service logs under `%ProgramData%\WinFreezeTrace\logs` by default

> Put `WinFreezeTrace.exe` in a permanent location such as `C:\WinFreezeTrace\WinFreezeTrace.exe` before running `install`. The Windows Service points to that exact EXE path, so do not move/delete it while installed.

## Build with Visual Studio / MSVC

From a Developer Command Prompt or a terminal with Visual Studio Build Tools installed:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Executable:

```text
build\Release\WinFreezeTrace.exe
```

## Recommended installation

Copy the built EXE to a permanent path:

```bat
mkdir C:\WinFreezeTrace
copy build\Release\WinFreezeTrace.exe C:\WinFreezeTrace\WinFreezeTrace.exe
cd /d C:\WinFreezeTrace
```

Then open **CMD as Administrator** once and run:

```bat
WinFreezeTrace.exe install
```

`install` does two things immediately:

1. registers `WinFreezeTrace` as an **Automatic** Windows Service
2. starts the service now

After that it starts automatically on every Windows boot.

## CMD controls

These commands can be run from CMD or PowerShell:

```bat
WinFreezeTrace.exe status
WinFreezeTrace.exe start
WinFreezeTrace.exe stop
WinFreezeTrace.exe restart
WinFreezeTrace.exe uninstall
WinFreezeTrace.exe logs
```

`install`, `start`, `stop`, `restart`, and `uninstall` may require an Administrator terminal because they control the Windows Service Control Manager.

Example status:

```text
Service: WinFreezeTrace
State: RUNNING
PID: 1234
Startup: Automatic
Binary: "C:\WinFreezeTrace\WinFreezeTrace.exe" service-run ...
```

## Install with custom monitor settings

All normal monitor options can be saved into the service configuration at install/update time:

```bat
WinFreezeTrace.exe install ^
  --interval 500 ^
  --ring-minutes 30 ^
  --pre-minutes 5 ^
  --post-minutes 5 ^
  --latency-ms 500 ^
  --queue 10
```

Custom service log path:

```bat
WinFreezeTrace.exe install --log-dir C:\WinFreezeTraceLogs
```

Running `install` again updates the existing service configuration instead of creating a duplicate service.

## Interactive / portable mode

The old behavior is still supported.

```bat
WinFreezeTrace.exe
```

or explicitly:

```bat
WinFreezeTrace.exe run
```

With options:

```bat
WinFreezeTrace.exe run --interval 500 --ring-minutes 30
```

Interactive mode defaults to:

```text
.\logs
```

Service mode defaults to:

```text
C:\ProgramData\WinFreezeTrace\logs
```

## What it records

Every 500 ms by default:

- CPU total
- DPC time
- Interrupt time
- Available RAM / Pages/sec
- process/thread count and uptime
- every `PhysicalDisk` PDH instance:
  - Avg. Disk sec/Read
  - Avg. Disk sec/Write
  - Current Disk Queue Length
  - Disk Bytes/sec
  - Disk Transfers/sec
  - % Disk Time
- top processes by read/write transfer rate
- collector call duration and sample gap

It also has a separate watchdog thread. If the collector heartbeat disappears for 2+ seconds, `watchdog.log` records the stage that was active (`pdh_collect`, `process_scan`, `file_write`, etc.).

## Trigger behavior

Default automatic trigger:

- any disk read/write latency >= 500 ms
- any disk queue >= 10
- collector heartbeat gap >= 2 seconds

On a trigger it creates:

```text
logs/event-YYYY-MM-DD_HH-MM-SS-mmm/
  metrics.csv
  summary.txt
  eventlog.xml
```

`metrics.csv` includes up to 5 minutes before the trigger from the in-memory ring buffer and 5 minutes after it.

`eventlog.xml` exports relevant System log events around the trigger for these providers when present: disk, storahci, Ntfs, WHEA-Logger, Display, nvlddmkm, Kernel-Power.

## Hard-reset survivability

`rolling.csv` is continuously written and flushed roughly every 2 seconds. It rotates at 32 MiB and keeps one previous file as `rolling.prev.csv`, so normal long-running capture is capped around 64 MiB plus event folders.

If Windows hard-freezes before an event can finish, send:

```text
rolling.csv
rolling.prev.csv
watchdog.log
errors.log
event-*/   (if any)
```

## Service recovery

The installed service is configured with Windows Service Recovery actions:

- first unexpected failure: restart after 5 seconds
- second: restart after 10 seconds
- third: restart after 30 seconds
- failure count resets after one day

A normal `WinFreezeTrace.exe stop` does not count as an unexpected crash.

## Uninstall

From Administrator CMD:

```bat
WinFreezeTrace.exe stop
WinFreezeTrace.exe uninstall
```

`uninstall` removes only the service registration. Existing diagnostic logs are intentionally kept.

## Important notes

- No repair operations are performed.
- No CHKDSK, filesystem repair, SMART writes, format, or disk configuration changes.
- Service mode runs as `LocalSystem`, which makes process/system telemetry more consistent than a normal user process.
- This version still does not automatically start WPR/ETW. That remains separate so the monitor itself stays lightweight.
- `PhysicalDisk(*)` names are PDH instance names such as `0 C:` or `_Total`.
