using Microsoft.UI.Dispatching;

namespace WinFreezeTraceManager.Services;

/// <summary>Polls the recorder once per second and raises events for the UI and the tray icon.</summary>
internal sealed class MonitorService
{
    private readonly DispatcherQueueTimer _timer;
    private bool _busy;
    private bool _everSeen;

    public CoreStatus Current { get; private set; } = CoreStatus.NotRunning;

    public event Action<CoreStatus>? StatusChanged;                 // every poll
    public event Action<string>? CaptureSaved;                      // folder of the new report
    public event Action? CaptureFailed;
    public event Action? RecorderLost;                              // it was running and disappeared without the Manager asking

    public MonitorService(DispatcherQueue queue)
    {
        _timer = queue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += async (_, _) => await PollAsync();
    }

    public void Start() { _timer.Start(); _ = PollAsync(); }
    public void Stop() => _timer.Stop();

    public async Task PollAsync()
    {
        if (_busy) return;
        _busy = true;
        try
        {
            var prev = Current;
            var next = await CoreClient.SendAsync("STATUS");
            Current = next;

            if (prev.Running && next.Running && prev.Pid == next.Pid)
            {
                if (next.Captures > prev.Captures) CaptureSaved?.Invoke(next.LastReport);
                else if (next.Failures > prev.Failures) CaptureFailed?.Invoke();
            }
            if (next.Running) _everSeen = true;
            if (prev.Running && !next.Running && !CoreLauncher.StopRequested && _everSeen) RecorderLost?.Invoke();
            StatusChanged?.Invoke(next);
        }
        catch (Exception ex) { Log.Error("poll failed: " + ex.Message); }
        finally { _busy = false; }
    }

    /// <summary>Sends a command and refreshes the state at once instead of waiting for the next tick.</summary>
    public async Task<CoreStatus> CommandAsync(string command)
    {
        var r = await CoreClient.SendAsync(command);
        await PollAsync();
        return r;
    }
}
