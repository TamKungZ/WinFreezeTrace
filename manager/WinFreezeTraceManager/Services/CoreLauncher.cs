using System.Diagnostics;

namespace WinFreezeTraceManager.Services;

/// <summary>Starts and stops the recorder process (WinFreezeTrace.exe + WinFreezeTraceCore.dll).</summary>
internal static class CoreLauncher
{
    /// <summary>True after the Manager itself asked the recorder to quit, so the monitor does not report a crash.</summary>
    public static bool StopRequested { get; private set; }

    public static string? FindExe(ManagerSettings settings)
    {
        var candidates = new List<string>();
        if (!string.IsNullOrWhiteSpace(settings.CoreExePath)) candidates.Add(settings.CoreExePath.Trim());
        var here = AppContext.BaseDirectory;
        candidates.Add(Path.Combine(here, "WinFreezeTrace.exe"));
        var parent = Directory.GetParent(here.TrimEnd(Path.DirectorySeparatorChar));
        if (parent != null) candidates.Add(Path.Combine(parent.FullName, "WinFreezeTrace.exe"));
        foreach (var c in candidates)
            if (File.Exists(c) && File.Exists(Path.Combine(Path.GetDirectoryName(c)!, "WinFreezeTraceCore.dll"))) return c;
        return null;
    }

    /// <summary>Returns null on success (or when it was already running), otherwise a message for the user.</summary>
    public static async Task<string?> StartAsync(ManagerSettings settings)
    {
        if ((await CoreClient.SendAsync("STATUS")).Running) return null;
        var exe = FindExe(settings);
        if (exe == null)
            return "WinFreezeTrace.exe and WinFreezeTraceCore.dll were not found next to the Manager. Reinstall WinFreezeTrace or set the recorder location in Settings.";

        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(exe)!,
        };
        foreach (var arg in settings.Recorder.BuildArguments()) psi.ArgumentList.Add(arg);
        try { Process.Start(psi)?.Dispose(); }
        catch (Exception ex)
        {
            Log.Error("could not start the recorder: " + ex.Message);
            return "Could not start the recorder: " + ex.Message;
        }

        for (int i = 0; i < 50; i++)
        {
            await Task.Delay(100);
            if ((await CoreClient.SendAsync("STATUS")).Running)
            {
                StopRequested = false;
                Log.Info("recorder started: " + exe);
                return null;
            }
        }
        Log.Warn("the recorder did not answer on its control pipe within 5 s");
        return "The recorder did not start. See winfreezetrace.log for details.";
    }

    public static async Task<string?> StopAsync()
    {
        StopRequested = true;
        var r = await CoreClient.SendAsync("EXIT");
        if (!r.Running) return null;                        // nothing was running
        for (int i = 0; i < 60; i++)                        // a capture in progress can take a few seconds
        {
            await Task.Delay(200);
            if (!(await CoreClient.SendAsync("STATUS")).Running) { Log.Info("recorder stopped"); return null; }
        }
        return "The recorder did not stop within 12 seconds (a report may still be being written).";
    }

    public static async Task<string?> RestartAsync(ManagerSettings settings)
    {
        var err = await StopAsync();
        if (err != null) return err;
        return await StartAsync(settings);
    }
}
