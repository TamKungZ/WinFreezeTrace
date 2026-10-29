# WinFreezeTrace

WinFreezeTrace is a Windows background recorder for investigating system stalls and unresponsive foreground apps. It keeps a rolling timeline in RAM and generates diagnostic reports when a stall is detected or you press `Ctrl+Alt+F12`.

Findings are heuristic, not proof of root cause. Use reports to guide further investigation with ETW/WPR or a debugger.

## Install

Download a release from the GitHub releases page:

- **Setup** (`WinFreezeTrace-Setup-<version>-<arch>.exe`): installs the recorder and, optionally, WinFreezeTrace Manager. Running a newer Setup over an existing install updates it.
- **Portable zip** (`WinFreezeTrace-<version>-win-<arch>.zip`): the recorder only, with the task scripts described below.

## Usage

Run `WinFreezeTrace.exe` to start monitoring. It runs in the background without a window or tray icon.

Press `Ctrl+Alt+F12` to capture a report manually, or let WinFreezeTrace capture one automatically when a trigger is detected.

Run `WinFreezeTrace.exe --help` to see available command-line options. `WinFreezeTrace.exe --exit` asks a running recorder to quit.

### WinFreezeTrace Manager

WinFreezeTrace Manager provides a tray icon and control window for managing the recorder without using command-line options.

- **Dashboard:** Check monitoring status and capture a report.
- **Reports:** Browse incidents, preview summaries, and open or delete reports.
- **Settings:** Configure the recorder (check interval, stall and hang thresholds, cooldown, history length, report limit, hotkey, memory dump, redaction), notifications, tray behavior, window material, and Start with Windows.

The Manager can start the recorder, and can notify you when a report is saved. Closing its window hides it to the tray; exiting the Manager does not stop the recorder unless you enable **Stop the recorder when the Manager exits**, or use **Stop the recorder completely** on the Dashboard.

The Manager is optional. You can run the recorder independently.

## Start with Windows

In WinFreezeTrace Manager, enable **Settings > Start with Windows** to launch the Manager in the background at login.

Without the Manager, run `install_task.cmd` to register a scheduled task that starts the recorder at logon. It copies the recorder to `%LOCALAPPDATA%\WinFreezeTrace\bin` and passes any options you give it, for example `install_task.cmd --redact --max-reports 20`. Run `uninstall_task.cmd` to remove the task and the copied files. Add `--purge` to also delete reports and logs.

## Capture Triggers

WinFreezeTrace captures a report when any of these conditions occurs:

- **Storage stall:** any physical disk averages at least 500 ms per I/O (`--disk-latency-ms`), or has a queue of at least 10 for two samples in a row (`--disk-queue`). Set either to `0` to turn it off. This catches the case where nothing is at 100% and the foreground app is not "Not Responding", yet clicks, audio and window switching all stall.
- **System stall:** the sampling loop is at least 1,500 ms late. This counts both a late wake-up and time spent blocked inside a counter read or the foreground probe.
- **App not responding:** the foreground window fails three consecutive 250 ms responsiveness probes.
- You press `Ctrl+Alt+F12`.

Automatic captures have a 60-second cooldown. Sleep gaps and stalls shortly after resume are filtered, but false positives and missed stalls are possible.

## Reports

Reports are saved to `%LOCALAPPDATA%\WinFreezeTrace\reports\` by default. Use `--report-dir` to change the report directory. Old reports are pruned beyond `--max-reports` (default 50).

Start with `summary.txt` for ranked findings, supporting evidence, and suggested next steps. `timeline.csv` contains the recorded history leading up to the trigger. Other files provide process, thread, module, wait-chain, Event Log, driver, storage, display, power, and system details. `summary.json` holds the same findings in machine-readable form.

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