# WinFreezeTrace

WinFreezeTrace is a Windows background recorder for investigating system stalls and unresponsive foreground apps. It keeps a rolling timeline in RAM and generates diagnostic reports on automatic triggers or `Ctrl+Alt+F12`.

Findings are heuristic, not proof of root cause. Use them to guide further investigation with ETW/WPR or a debugger.

## Usage

Run `WinFreezeTrace.exe` to start recording. Press `Ctrl+Alt+F12` to capture a report manually.

WinFreezeTrace Manager provides tray controls, report browsing, and recorder settings. It is optional; the recorder can run independently.

By default, reports are saved to `%LOCALAPPDATA%\WinFreezeTrace\reports\`. Start with `summary.txt` for findings and suggested next steps. See `timeline.csv` and the other report files for supporting data.

Use `--redact` before sharing reports. Redaction does not cover `target.dmp`; use `--no-dump` to avoid creating a process dump. Reports may contain sensitive system data.

## Capture Triggers

- Watchdog wake-up is at least 1,500 ms late.
- The foreground window fails three consecutive 250 ms responsiveness probes.
- Manual capture via the hotkey.

Automatic captures have a 60-second cooldown. False positives and missed stalls are possible.

## Limitations

Monitoring samples system counters and probes the foreground window. Captures are more expensive and may briefly suspend the foreground process when creating a dump. Anti-cheat or security software may flag process inspection.

Hard system locks may prevent reports from being written, and short spikes or some application and GPU hangs may go undetected. Zero performance impact is not guaranteed.

## Build and Test

Requires CMake and a supported C/C++ toolchain. The Manager additionally requires the .NET 8 SDK and Windows App SDK workload on Windows.

See `CHANGELOG.md` for release-specific changes. Licensed under 0BSD (see `LICENSE`).