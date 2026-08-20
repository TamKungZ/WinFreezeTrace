# WinFreezeTrace

Native Windows C black-box recorder for intermittent freezes / storage stalls.

Version: 0.1.0

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

`logs/rolling.csv` is continuously written and flushed roughly every 2 seconds. It rotates at 32 MiB and keeps one previous file as `rolling.prev.csv`, so normal long-running capture is capped around 64 MiB plus event folders.

If Windows hard-freezes before an event can finish, send:

```text
logs/rolling.csv
logs/rolling.prev.csv
logs/watchdog.log
logs/errors.log
logs/event-*/   (if any)
```

## Build with Visual Studio / MSVC

From a Developer Command Prompt or normal terminal with Visual Studio Build Tools installed:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Executable:

```text
build\Release\WinFreezeTrace.exe
```

## Build with Ninja + MSVC

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

Normal:

```bat
WinFreezeTrace.exe
```

Recommended for this freeze investigation:

```bat
WinFreezeTrace.exe --interval 500 --ring-minutes 30 --pre-minutes 5 --post-minutes 5 --latency-ms 500 --queue 10
```

Put logs somewhere else if desired:

```bat
WinFreezeTrace.exe --log-dir D:\WinFreezeTraceLogs
```

For the cleanest C:-only hardware test, keep the default local `logs` directory on C: so disconnected drives are not touched.

## Important notes

- No repair operations are performed.
- No CHKDSK, filesystem changes, SMART writes, or disk configuration changes.
- Administrator rights are normally not required for the core PDH capture. Some processes may deny I/O-query access; they are simply skipped.
- This first version does not automatically start WPR/ETW. That is intentionally separate so the initial monitor adds as little behavior as possible to the machine being diagnosed.
- `PhysicalDisk(*)` names are PDH instance names such as `0 C:` or `_Total`.
