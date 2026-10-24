# WinFreezeTrace

WinFreezeTrace is a Windows background recorder for investigating system stalls and unresponsive foreground apps. It keeps a short timeline in RAM and writes a diagnostic report after an automatic trigger or `Ctrl+Alt+F12`.

Findings are heuristic, not proof of root cause. Use the report to guide further investigation with tools such as ETW/WPR or a debugger.

## Run

```bat
WinFreezeTrace.exe
WinFreezeTrace.exe --help
```

Default manual capture hotkey: `Ctrl+Alt+F12`.

### WinFreezeTrace Manager (tray icon and control window)

The recorder (`WinFreezeTrace.exe` + `WinFreezeTraceCore.dll`) has **no window and no tray icon**: it only monitors and captures. Everything you interact with lives in a separate WinUI 3 program, **WinFreezeTrace Manager** (`Manager\WinFreezeTraceManager.exe`), which is installed by default (the installer lets you untick it, or pass `/MANAGER=0` to a silent install).

| Tray icon | Meaning |
| --- | --- |
| Green check badge | Monitoring is **on** |
| Grey pause badge | Monitoring is **off**: no sampling, no probing, no captures, no hotkey |
| Orange dot badge | A report is being written, or the recorder is not running (grey) |

- **Left-click** the tray icon to open the window; **right-click** for the menu (status, turn monitoring on/off, capture now, open reports folder, Start with Windows, Exit, Exit and stop the recorder).
- **Dashboard**: state, on/off, *Capture report now*, latest report. **Reports**: browse, preview `summary.txt`, open or delete incidents. **Settings**: start with Windows, recorder options (interval, triggers, dump, redaction, report folder), *Save and restart the recorder*.
- The Manager starts the recorder (with the options from Settings) if it is not running, and talks to it over a per-user named pipe. Closing the window hides it to the tray; exiting the Manager leaves the recorder running unless you choose *Exit and stop the recorder*.
- Turning monitoring off discards the in-memory history, so the baseline in the next report never spans the gap.

The recorder works without the Manager. Run it directly, from `install_task.cmd`, or from the all-users startup entry; use `WinFreezeTrace.exe --exit` to stop it.

### Start with Windows

Use *Settings > Start with Windows* in the Manager. It adds a per-user entry for the Manager (visible under **Settings > Apps > Startup** and Task Manager > Startup) that starts hidden in the tray and then starts the recorder. The old `--add-startup` / `--remove-startup` recorder options are gone; an entry made by 0.5.x is migrated the first time the Manager runs.

`install_task.cmd` is an alternative that registers a scheduled task with automatic restart of the recorder alone; having both is harmless because only one recorder runs per session. The installer has an optional \"Start with Windows for all users\" component (it starts the Manager if installed, otherwise the recorder).

For lower capture impact and no process dump:

```bat
WinFreezeTrace.exe --interval 2000 --no-dump
```

To run the recorder alone at logon, execute `install_task.cmd`. Use `uninstall_task.cmd` to remove the task, executable and DLL, or `uninstall_task.cmd --purge` to delete reports and logs too.

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

Recorder and tests (as below), then the Manager (needs the .NET 8 SDK and the Windows App SDK workload on Windows):

```bat
dotnet publish manager\WinFreezeTraceManager\WinFreezeTraceManager.csproj -c Release -r win-x64 -p:Platform=x64 -o manager-out
```

The Manager is self-contained; copy `manager-out` next to the recorder as a `Manager` folder.

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
