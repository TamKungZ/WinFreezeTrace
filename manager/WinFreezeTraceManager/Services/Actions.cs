using System.Diagnostics;

namespace WinFreezeTraceManager.Services;

/// <summary>Operations shared by the window, the tray menu and the settings page. Each returns null on success or a user-readable error.</summary>
internal static class Actions
{
    public static async Task<string?> StartRecorderAsync()
    {
        var err = await CoreLauncher.StartAsync(App.Settings);
        await App.Monitor.PollAsync();
        return err;
    }

    public static async Task<string?> StopRecorderAsync()
    {
        var err = await CoreLauncher.StopAsync();
        await App.Monitor.PollAsync();
        return err;
    }

    public static async Task<string?> RestartRecorderAsync()
    {
        var err = await CoreLauncher.RestartAsync(App.Settings);
        await App.Monitor.PollAsync();
        return err;
    }

    /// <summary>Not running -> start; running and on -> off; running and off -> on.</summary>
    public static async Task<string?> ToggleMonitoringAsync()
    {
        var s = App.Monitor.Current;
        if (!s.Running) return await StartRecorderAsync();
        var r = await App.Monitor.CommandAsync(s.Paused ? "RESUME" : "PAUSE");
        return r.Running ? (r.Ok ? null : r.Error) : "The recorder is not running.";
    }

    public static async Task<string?> CaptureNowAsync()
    {
        var r = await App.Monitor.CommandAsync("CAPTURE");
        if (!r.Running) return "The recorder is not running.";
        return r.Ok ? null : "Capture was not started: " + r.Error + ".";
    }

    public static string ReportsDir()
    {
        var s = App.Monitor.Current;
        if (s.Running && s.ReportsDir.Length > 0) return s.ReportsDir;
        var custom = App.Settings.Recorder.ReportDir;
        return custom.Length > 0 ? custom : AppPaths.DefaultReportsDir;
    }

    public static void OpenFolder(string path)
    {
        try
        {
            if (!Directory.Exists(path)) { Log.Warn("folder does not exist: " + path); return; }
            var psi = new ProcessStartInfo("explorer.exe") { UseShellExecute = false };
            psi.ArgumentList.Add(path);
            Process.Start(psi)?.Dispose();
        }
        catch (Exception ex) { Log.Warn("could not open folder: " + ex.Message); }
    }

    public static void OpenFile(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true })?.Dispose();
        }
        catch (Exception ex) { Log.Warn("could not open file: " + ex.Message); }
    }

    public static void OpenUrl(string url)
    {
        try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })?.Dispose(); }
        catch (Exception ex) { Log.Warn("could not open link: " + ex.Message); }
    }
}
