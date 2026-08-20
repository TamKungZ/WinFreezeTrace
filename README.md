# WinFreezeTrace

Native Windows C black-box recorder for intermittent freezes / storage stalls.

Version: **0.2.1**

## Global CLI + background service

`install` now installs WinFreezeTrace like a normal command-line tool:

- copies the executable to `C:\Program Files\WinFreezeTrace\winfreezetrace.exe`
- adds `C:\Program Files\WinFreezeTrace` to the **Machine PATH** using the Windows Registry API
- installs the `WinFreezeTrace` Windows Service
- configures the service as **Automatic** at Windows boot
- starts the service immediately
- keeps background service logs under `%ProgramData%\WinFreezeTrace\logs`

After installation, open a **new** CMD or PowerShell window. You can then call it from any directory just like `cargo`, `python`, `pip`, or `gradle`:

```bat
winfreezetrace status
winfreezetrace logs
winfreezetrace restart
```

The PATH is changed system-wide. An already-open CMD cannot have its parent environment modified by a child process, so a new terminal is required once after install/uninstall.

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

## Install

Open **CMD as Administrator** from the project directory and run:

```bat
build\Release\WinFreezeTrace.exe install
```

You do **not** need to manually copy the EXE anywhere.

Then close that terminal and open a new CMD/PowerShell:

```bat
winfreezetrace status
```

You can verify command discovery with:

```bat
where winfreezetrace
```

Expected path:

```text
C:\Program Files\WinFreezeTrace\winfreezetrace.exe
```

## Global CLI commands

```bat
winfreezetrace status
winfreezetrace start
winfreezetrace stop
winfreezetrace restart
winfreezetrace logs
winfreezetrace uninstall
winfreezetrace --help
```

`install`, `start`, `stop`, `restart`, and `uninstall` can require an Administrator terminal because they control Windows Service configuration.

## Install/update with custom monitor settings

```bat
WinFreezeTrace.exe install ^
  --interval 500 ^
  --ring-minutes 30 ^
  --pre-minutes 5 ^
  --post-minutes 5 ^
  --latency-ms 500 ^
  --queue 10
```

After the first installation, an update can also be run from a newly-built EXE. `install` will stop the old service before replacing the installed binary, update its configuration, and start it again.

Custom service log path:

```bat
WinFreezeTrace.exe install --log-dir C:\WinFreezeTraceLogs
```

## Background behavior

The service:

- starts automatically at Windows boot
- does not require user login
- has no console window while monitoring
- runs independently of the CLI terminal
- restarts after an unexpected service failure

Default service logs:

```text
C:\ProgramData\WinFreezeTrace\logs
```

Print the path at any time:

```bat
winfreezetrace logs
```

## Interactive / portable mode

The executable can still be used without installing it:

```bat
WinFreezeTrace.exe
```

or:

```bat
WinFreezeTrace.exe run
```

With options:

```bat
WinFreezeTrace.exe run --interval 500 --ring-minutes 30
```

Interactive mode defaults to `./logs`; service mode defaults to `%ProgramData%\WinFreezeTrace\logs`.

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

A separate watchdog thread checks the collector heartbeat. If the collector disappears for 2+ seconds, `watchdog.log` records the stage that was active, such as `pdh_collect`, `process_scan`, or `file_write`.

## Trigger behavior

Default trigger conditions:

- any disk read/write latency >= 500 ms
- any disk queue >= 10
- collector heartbeat gap >= 2 seconds

A trigger creates:

```text
logs/event-YYYY-MM-DD_HH-MM-SS-mmm/
  metrics.csv
  summary.txt
  eventlog.xml
```

`metrics.csv` includes up to 5 minutes before the trigger from the in-memory ring buffer and 5 minutes after it.

`eventlog.xml` exports relevant System log events around the trigger for providers such as disk, storahci, Ntfs, WHEA-Logger, Display, nvlddmkm, and Kernel-Power when present.

## Hard-reset survivability

`rolling.csv` is continuously written and flushed roughly every 2 seconds. It rotates at 32 MiB and keeps `rolling.prev.csv`, so normal long-running capture is capped around 64 MiB plus event folders.

If Windows hard-freezes before an event can finish, preserve:

```text
rolling.csv
rolling.prev.csv
watchdog.log
errors.log
event-*/
```

## Service recovery

The installed service uses Windows Service Recovery:

- first unexpected failure: restart after 5 seconds
- second: restart after 10 seconds
- third: restart after 30 seconds
- failure count resets after one day

A normal `winfreezetrace stop` is not an unexpected crash.

## Uninstall

From Administrator CMD/PowerShell:

```bat
winfreezetrace uninstall
```

This removes:

- the Windows Service
- the Machine PATH entry
- the installed CLI executable

If `uninstall` is currently executing from the installed binary, Windows schedules that EXE for deletion at the next reboot because a running executable cannot safely delete itself.

Diagnostic logs are intentionally kept.

## Important notes

- No CHKDSK, filesystem repair, SMART writes, format, or disk configuration changes are performed.
- Machine PATH is edited through the Registry API rather than `setx`, avoiding `setx` PATH truncation/expansion issues.
- Service mode runs as `LocalSystem`.
- WPR/ETW automatic recording is still intentionally separate so this monitor remains lightweight.
- `PhysicalDisk(*)` names are PDH instance names such as `0 C:` or `_Total`.
