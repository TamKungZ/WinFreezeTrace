# WinFreezeTrace

WinFreezeTrace is a Windows background recorder for investigating system stalls and unresponsive foreground apps. It keeps a short timeline in RAM and writes a diagnostic report after an automatic trigger or `Ctrl+Alt+F12`.

Findings are heuristic, not proof of root cause. Use the report to guide further investigation with tools such as ETW/WPR or a debugger.

## Run

```bat
WinFreezeTrace.exe
WinFreezeTrace.exe --help
```

Default manual capture hotkey: `Ctrl+Alt+F12`.

### Tray icon

While running, WinFreezeTrace shows an icon in the notification area (click `^` if Windows hides it):

| Icon | Meaning |
| --- | --- |
| Green check | Monitoring is **on** |
| Grey pause bars | Monitoring is **off**: no sampling, no probing, no captures, no hotkey |
| Orange dot | A report is being written |

- **Left-click** the icon to turn monitoring on or off (a double-click toggles only once).
- **Right-click** for the menu: current status, *Turn monitoring ON/OFF*, *Start with Windows*, *Open reports folder*, *Exit*.
- After a capture a balloon appears; click it to open the report folder.
- Turning monitoring off discards the in-memory history, so the baseline in the next report never spans the gap.
- `--no-tray` runs without the icon, `--no-notify` suppresses balloons. A second launch just tells the running copy to show a "already running" balloon.

### Start with Windows

Use *Start with Windows* in the tray menu, or from a terminal:

```bat
WinFreezeTrace.exe --add-startup --interval 2000
WinFreezeTrace.exe --remove-startup
WinFreezeTrace.exe --exit
```

This adds a per-user entry that appears under **Settings > Apps > Startup** (and Task Manager > Startup), where it can also be switched off; the other options you pass are stored with it. `--exit` asks the running copy of this session to quit. `install_task.cmd` is an alternative that registers a scheduled task with automatic restart; having both is harmless because only one instance runs per session. The installer has an optional "Start with Windows for all users" component.

For lower capture impact and no process dump:

```bat
WinFreezeTrace.exe --interval 2000 --no-dump
```

To run at logon, execute `install_task.cmd`. Use `uninstall_task.cmd` to remove the task, executable and DLL, or `uninstall_task.cmd --purge` to delete reports and logs too.

## Capture triggers

- Watchdog wake-up is at least 1,500 ms late.
- The foreground window fails a 250 ms `WM_NULL` responsiveness probe three times in a row.
- Manual capture via the hotkey.

Automatic captures have a 60-second cooldown. Sleep gaps and stalls shortly after resume are filtered, but false positives and negatives remain possible.

## Reports

Reports are saved under `%LOCALAPPDATA%\WinFreezeTrace\reports\` by default; set `--report-dir` to change the root.

Start with `summary.txt` for ranked findings, evidence, and next steps. Use `timeline.csv` for pre-trigger history and the other files for process, thread, wait-chain, Event Log, driver, storage, display, power, startup-program (`startup_apps.csv`), and system details. Process CPU/I/O rates in the report are measured just after the trigger, not before it.

Use `--redact` before sharing reports. It masks common identifying fields and omits raw Event XML, but **does not redact `target.dmp`**; add `--no-dump` to avoid creating that file. Reports and dumps may contain sensitive system and process data.

## Overhead and limitations

Normal monitoring samples system counters and probes the foreground window; it does not continuously run ETW/WPR or create dumps. The probe can wait up to `--hang-timeout-ms` inside WinFreezeTrace.

Capture is more expensive than monitoring. Creating `target.dmp` suspends the foreground process while the dump is written and may cause a hitch. Anti-cheat or security software may also flag process inspection, wait-chain traversal, or dump creation. Test on the target system before gaming or other latency-sensitive use; zero performance impact is not guaranteed.

A hard system lock may prevent a report from being written, and one-second sampling can miss short spikes. `WM_NULL` does not detect every type of app or GPU hang. A report with no findings does not establish that the system is healthy.

## Build and test

Visual Studio Developer Command Prompt:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

MinGW-w64:

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build
```

Core tests on supported platforms:

```sh
./tests/run.sh
```

See `CHANGELOG.md` for release-specific changes. Licensed under 0BSD (see `LICENSE`).
