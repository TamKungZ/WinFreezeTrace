# WinFreezeTrace

A lightweight native Windows background monitor for capturing system performance data around freezes, hangs, storage stalls, and unexplained system slowdowns.

WinFreezeTrace runs continuously as a Windows service and keeps a rolling history of system metrics. When abnormal disk latency, queue depth, or collector stalls are detected, it preserves diagnostic data around the event for later analysis.

Written in C with no external runtime dependencies.

## Features

* Native C implementation
* Runs as a Windows Service
* Starts automatically with Windows
* Global `winfreezetrace` CLI
* Low-overhead 500 ms sampling
* Rolling log with bounded disk usage
* In-memory ring buffer
* Automatic freeze/stall detection
* Per-physical-disk performance counters
* CPU, DPC, interrupt, memory, and paging metrics
* Process I/O monitoring
* Collector heartbeat monitoring
* Windows Event Log correlation
* Automatic service recovery after crashes
* No filesystem repair or disk modification

## Metrics

WinFreezeTrace records:

### System

* CPU usage
* DPC time
* Interrupt time
* Available memory
* Paging activity
* Process count
* Thread count
* System uptime

### Physical disks

* Read latency
* Write latency
* Disk queue length
* Read/write throughput
* IOPS
* Disk busy percentage

### Processes

* Process I/O activity
* High-I/O processes

### Monitor health

* Collector heartbeat
* Sampling gaps
* PDH query duration
* Process scan duration
* File write duration

This helps distinguish an application slowdown from a storage stall, driver/kernel delay, or a broader system freeze.

## Requirements

* Windows 10 or Windows 11
* CMake
* Visual Studio / MSVC Build Tools

## Build

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be available at:

```text
build\Release\WinFreezeTrace.exe
```

## Install

Open CMD or PowerShell as Administrator:

```bat
build\Release\WinFreezeTrace.exe install
```

WinFreezeTrace installs itself to:

```text
C:\Program Files\WinFreezeTrace\winfreezetrace.exe
```

and adds the directory to the system `PATH`.

Open a new terminal after installation.

You can then run:

```bat
winfreezetrace status
```

from any directory.

The Windows service is configured to start automatically during boot.

## CLI

```text
winfreezetrace status
winfreezetrace start
winfreezetrace stop
winfreezetrace restart
winfreezetrace logs
winfreezetrace uninstall
winfreezetrace --help
```

`status`, `logs`, and help commands can be used without administrator privileges.

Commands that modify the Windows service require an elevated terminal.

## Default Configuration

```text
Sampling interval       500 ms
Ring buffer             30 minutes
Pre-trigger history      5 minutes
Post-trigger capture     5 minutes
Disk latency trigger   500 ms
Disk queue trigger       10
Collector gap trigger     2 seconds
```

Custom values can be specified during installation:

```bat
WinFreezeTrace.exe install ^
  --interval 500 ^
  --ring-minutes 30 ^
  --pre-minutes 5 ^
  --post-minutes 5 ^
  --latency-ms 500 ^
  --queue 10
```

Running `install` again updates the existing service configuration and restarts it.

## Logs

Logs are stored in:

```text
C:\ProgramData\WinFreezeTrace\logs
```

Open the log directory with:

```bat
winfreezetrace logs
```

Typical output:

```text
logs\
├── rolling.csv
├── rolling.prev.csv
├── watchdog.log
├── errors.log
└── event-YYYY-MM-DD_HH-MM-SS-mmm\
    ├── metrics.csv
    ├── summary.txt
    └── eventlog.xml
```

### `rolling.csv`

Continuously records recent performance metrics.

The rolling log has a bounded size so WinFreezeTrace can remain active for long periods without continuously consuming disk space.

### `watchdog.log`

Records collector heartbeat information and sampling stalls.

A gap here can be useful when the system becomes too unresponsive for normal performance queries to complete.

### Event directories

When an abnormal condition is detected, WinFreezeTrace preserves metrics from before and after the trigger in a separate event directory.

Relevant Windows Event Log entries are also collected when possible.

## Event Detection

An event can be triggered by conditions such as:

```text
Disk latency >= 500 ms
Disk queue   >= 10
Collector heartbeat gap >= 2 seconds
```

The purpose of these triggers is not to declare a component faulty. They preserve the surrounding timeline so the actual cause can be investigated afterward.

For example:

```text
T - 30s   PhysicalDisk latency begins increasing
T - 10s   Disk queue rises
T -  3s   DPC activity spikes
T         Desktop stops responding
```

This is generally more useful than inspecting Task Manager after the system has already recovered.

## Uninstall

Open an elevated terminal:

```bat
winfreezetrace uninstall
```

This removes the Windows service, installed CLI, and PATH entry.

Diagnostic logs are preserved.