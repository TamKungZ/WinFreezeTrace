# WinFreezeTrace

WinFreezeTrace is a Windows background recorder for investigating system stalls and unresponsive foreground apps. It keeps a rolling timeline in RAM and generates diagnostic reports when a stall is detected or you press `Ctrl+Alt+F12`.

Findings are heuristic, not proof of root cause. Use reports to guide further investigation with ETW/WPR or a debugger.

## Usage

Run `WinFreezeTrace.exe` to start monitoring. It runs in the background without a window or tray icon.

Press `Ctrl+Alt+F12` to capture a report manually, or let WinFreezeTrace capture one automatically when a trigger is detected.

Run `WinFreezeTrace.exe --help` to see available command-line options.

### WinFreezeTrace Manager

WinFreezeTrace Manager provides a tray icon and control window for managing the recorder without using command-line options.

- **Dashboard:** Check monitoring status and capture a report.
- **Reports:** Browse incidents, preview summaries, and open or delete reports.
- **Settings:** Configure monitoring, capture triggers, dump creation, redaction, report location, and startup behavior.

The Manager starts the recorder when needed. Closing its window hides it to the tray; exiting the Manager does not stop the recorder unless you choose **Exit and stop the recorder**.

The Manager is optional. You can run the recorder independently.

## Start with Windows

In WinFreezeTrace Manager, enable **Settings > Start with Windows** to launch the Manager in the background at login.

Alternatively, run `install_task.cmd` to register a scheduled task that starts the recorder and restarts it if needed. Run `uninstall_task.cmd` to remove the task and recorder files. Add `--purge` to also delete reports and logs.

To reduce monitoring frequency and disable process dumps, run:

```bat
WinFreezeTrace.exe --interval 2000 --no-dump
```

## Capture Triggers

WinFreezeTrace captures a report when any of these conditions occurs:

- The watchdog wakes up at least 1,500 ms late.
- The foreground window fails three consecutive 250 ms responsiveness probes.
- You press `Ctrl+Alt+F12`.

Automatic captures have a 60-second cooldown. Sleep gaps and stalls shortly after resume are filtered, but false positives and missed stalls are possible.

## Reports

Reports are saved to `%LOCALAPPDATA%\WinFreezeTrace\reports\` by default. Use `--report-dir` to change the report directory.

Start with `summary.txt` for ranked findings, supporting evidence, and suggested next steps. `timeline.csv` contains the recorded history leading up to the trigger. Other files provide process, thread, wait-chain, Event Log, driver, storage, display, power, startup-program, and system details.

Process CPU and I/O rates are measured just after the trigger, not before it.

Before sharing reports, use `--redact` to mask common identifying fields and omit raw Event XML. Redaction does not cover `target.dmp`; use `--no-dump` to avoid creating a dump. Reports and dumps may contain sensitive system and process data.

## Overhead and Limitations

Normal monitoring samples system counters and probes the foreground window. It does not continuously run ETW/WPR or create process dumps.

Captures require more resources than monitoring. Creating `target.dmp` suspends the foreground process while the dump is written and may cause a temporary hitch. Anti-cheat or security software may also flag process inspection, wait-chain traversal, or dump creation.

A hard system lock may prevent a report from being written. One-second sampling can miss short spikes, and `WM_NULL` responsiveness probes cannot detect every type of application or GPU hang. A report with no findings does not establish that the system is healthy.

## Build and Test

Build the recorder and run its tests with CMake:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

For MinGW-w64, use `-G "MinGW Makefiles"` instead of `-A x64`, then run:

```bat
cmake --build build
ctest --test-dir build
```

Core tests can also be run on supported platforms with:

```sh
./tests/run.sh
```

The Manager requires the .NET 8 SDK and Windows App SDK workload on Windows. Publish it with:

```bat
dotnet publish manager\WinFreezeTraceManager\WinFreezeTraceManager.csproj -c Release -r win-x64 -p:Platform=x64 -o manager-out
```

Copy the published output next to the recorder in a `Manager` folder.

See `CHANGELOG.md` for release-specific changes. Licensed under 0BSD (see `LICENSE`).