using System.Text.Json;

namespace WinFreezeTraceManager.Services;

/// <summary>Options passed to the recorder (WinFreezeTrace.exe). Ranges mirror src/core/config.c.</summary>
internal sealed class RecorderOptions
{
    public int IntervalMs { get; set; } = 1000;        // 250..60000
    public int HistorySeconds { get; set; } = 180;     // 30..600
    public int StallMs { get; set; } = 1500;           // 500..30000
    public int HangTimeoutMs { get; set; } = 250;      // 50..5000
    public int HangSamples { get; set; } = 3;          // 2..20
    public int CooldownSeconds { get; set; } = 60;     // 5..86400
    public int LeadSeconds { get; set; } = 30;         // 5..300
    public int MaxReports { get; set; } = 50;          // 0..5000 (0 = unlimited)
    public int MinFreeMb { get; set; } = 1024;         // 0..1048576 (0 = off)
    public int DumpTimeoutMs { get; set; } = 15000;    // 1000..120000
    public bool MakeDump { get; set; } = true;
    public bool Redact { get; set; } = false;
    public bool Hotkey { get; set; } = true;
    public string ReportDir { get; set; } = "";         // empty = recorder default

    public void Clamp()
    {
        IntervalMs = Math.Clamp(IntervalMs, 250, 60000);
        HistorySeconds = Math.Clamp(HistorySeconds, 30, 600);
        StallMs = Math.Clamp(StallMs, 500, 30000);
        HangTimeoutMs = Math.Clamp(HangTimeoutMs, 50, 5000);
        HangSamples = Math.Clamp(HangSamples, 2, 20);
        CooldownSeconds = Math.Clamp(CooldownSeconds, 5, 86400);
        LeadSeconds = Math.Clamp(LeadSeconds, 5, 300);
        MaxReports = Math.Clamp(MaxReports, 0, 5000);
        MinFreeMb = Math.Clamp(MinFreeMb, 0, 1048576);
        DumpTimeoutMs = Math.Clamp(DumpTimeoutMs, 1000, 120000);
        ReportDir = (ReportDir ?? "").Trim();
    }

    /// <summary>One list item per argument; ProcessStartInfo.ArgumentList does the quoting.</summary>
    public List<string> BuildArguments()
    {
        Clamp();
        var a = new List<string>
        {
            "--interval", IntervalMs.ToString(),
            "--history", HistorySeconds.ToString(),
            "--stall-ms", StallMs.ToString(),
            "--hang-timeout-ms", HangTimeoutMs.ToString(),
            "--hang-samples", HangSamples.ToString(),
            "--cooldown", CooldownSeconds.ToString(),
            "--lead-window", LeadSeconds.ToString(),
            "--max-reports", MaxReports.ToString(),
            "--min-free-mb", MinFreeMb.ToString(),
            "--dump-timeout-ms", DumpTimeoutMs.ToString(),
        };
        if (!MakeDump) a.Add("--no-dump");
        if (Redact) a.Add("--redact");
        if (!Hotkey) a.Add("--no-hotkey");
        if (ReportDir.Length > 0) { a.Add("--report-dir"); a.Add(ReportDir); }
        return a;
    }
}

internal sealed class ManagerSettings
{
    public string CoreExePath { get; set; } = "";          // empty = look next to the Manager
    public bool AutoStartRecorder { get; set; } = true;     // start the recorder when the Manager starts
    public bool CloseToTray { get; set; } = true;           // the close button hides the window
    public bool ShowNotifications { get; set; } = true;     // balloon when a report is saved
    public bool StopRecorderOnExit { get; set; } = false;   // plain "Exit" also stops the recorder
    public RecorderOptions Recorder { get; set; } = new();

    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    public static ManagerSettings Load()
    {
        try
        {
            if (File.Exists(AppPaths.SettingsFile))
            {
                var s = JsonSerializer.Deserialize<ManagerSettings>(File.ReadAllText(AppPaths.SettingsFile));
                if (s != null) { s.Recorder ??= new RecorderOptions(); s.Recorder.Clamp(); return s; }
            }
        }
        catch (Exception ex) { Log.Warn("settings could not be read, using defaults: " + ex.Message); }
        return new ManagerSettings();
    }

    public bool Save(out string? error)
    {
        try
        {
            Recorder.Clamp();
            Directory.CreateDirectory(AppPaths.DataDir);    // normally the recorder has already created it (private ACL)
            var tmp = AppPaths.SettingsFile + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, Json));
            File.Move(tmp, AppPaths.SettingsFile, overwrite: true);
            error = null;
            return true;
        }
        catch (Exception ex)
        {
            Log.Error("settings could not be saved: " + ex.Message);
            error = ex.Message;
            return false;
        }
    }
}
