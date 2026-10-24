using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager.Pages;

public sealed partial class SettingsPage : Page
{
    private bool _loading;

    public SettingsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => LoadFromSettings();
    }

    private void LoadFromSettings()
    {
        _loading = true;
        try
        {
            var s = App.Settings;
            StartupSwitch.IsOn = StartupRegistration.IsEnabled();
            AutoStartSwitch.IsOn = s.AutoStartRecorder;
            CloseToTraySwitch.IsOn = s.CloseToTray;
            NotifySwitch.IsOn = s.ShowNotifications;
            StopOnExitSwitch.IsOn = s.StopRecorderOnExit;
            CoreExeBox.Text = s.CoreExePath;
            ShowRecorderOptions(s.Recorder);
            UpdateCoreFound();
        }
        finally { _loading = false; }
    }

    private void ShowRecorderOptions(RecorderOptions r)
    {
        IntervalBox.Value = r.IntervalMs;
        HistoryBox.Value = r.HistorySeconds;
        StallBox.Value = r.StallMs;
        HangTimeoutBox.Value = r.HangTimeoutMs;
        HangSamplesBox.Value = r.HangSamples;
        CooldownBox.Value = r.CooldownSeconds;
        LeadBox.Value = r.LeadSeconds;
        MaxReportsBox.Value = r.MaxReports;
        MinFreeBox.Value = r.MinFreeMb;
        DumpTimeoutBox.Value = r.DumpTimeoutMs;
        DumpSwitch.IsOn = r.MakeDump;
        RedactSwitch.IsOn = r.Redact;
        HotkeySwitch.IsOn = r.Hotkey;
        ReportDirBox.Text = r.ReportDir;
    }

    private static int Val(NumberBox box, int fallback) => double.IsNaN(box.Value) ? fallback : (int)Math.Round(box.Value);

    private void ReadRecorderOptions()
    {
        var r = App.Settings.Recorder;
        r.IntervalMs = Val(IntervalBox, r.IntervalMs);
        r.HistorySeconds = Val(HistoryBox, r.HistorySeconds);
        r.StallMs = Val(StallBox, r.StallMs);
        r.HangTimeoutMs = Val(HangTimeoutBox, r.HangTimeoutMs);
        r.HangSamples = Val(HangSamplesBox, r.HangSamples);
        r.CooldownSeconds = Val(CooldownBox, r.CooldownSeconds);
        r.LeadSeconds = Val(LeadBox, r.LeadSeconds);
        r.MaxReports = Val(MaxReportsBox, r.MaxReports);
        r.MinFreeMb = Val(MinFreeBox, r.MinFreeMb);
        r.DumpTimeoutMs = Val(DumpTimeoutBox, r.DumpTimeoutMs);
        r.MakeDump = DumpSwitch.IsOn;
        r.Redact = RedactSwitch.IsOn;
        r.Hotkey = HotkeySwitch.IsOn;
        r.ReportDir = ReportDirBox.Text.Trim();
        r.Clamp();
        App.Settings.CoreExePath = CoreExeBox.Text.Trim();
    }

    private void UpdateCoreFound()
    {
        var exe = CoreLauncher.FindExe(App.Settings);
        CoreFound.Text = exe == null
            ? "Not found: WinFreezeTrace.exe (with WinFreezeTraceCore.dll beside it) must be next to the Manager, one folder above it, or at the location above."
            : "Using " + exe;
    }

    private void Info(string message, InfoBarSeverity severity)
    {
        Notice.Severity = severity;
        Notice.Message = message;
        Notice.IsOpen = true;
    }

    // ---- Manager options: applied immediately ----

    private void Startup_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (!StartupRegistration.Set(StartupSwitch.IsOn, out var err))
        {
            Info("Could not change the Windows Startup setting: " + err, InfoBarSeverity.Error);
            _loading = true; StartupSwitch.IsOn = StartupRegistration.IsEnabled(); _loading = false;
        }
    }

    private void General_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var s = App.Settings;
        s.AutoStartRecorder = AutoStartSwitch.IsOn;
        s.CloseToTray = CloseToTraySwitch.IsOn;
        s.ShowNotifications = NotifySwitch.IsOn;
        s.StopRecorderOnExit = StopOnExitSwitch.IsOn;
        if (!s.Save(out var err)) Info("Settings could not be saved: " + err, InfoBarSeverity.Error);
    }

    // ---- Recorder options: saved on request ----

    private async void Browse_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker();
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.WindowHandle);
            picker.FileTypeFilter.Add("*");
            var folder = await picker.PickSingleFolderAsync();
            if (folder != null) ReportDirBox.Text = folder.Path;
        }
        catch (Exception ex) { Info("The folder picker failed: " + ex.Message, InfoBarSeverity.Error); }
    }

    private bool SaveAll()
    {
        ReadRecorderOptions();
        if (!App.Settings.Save(out var err)) { Info("Settings could not be saved: " + err, InfoBarSeverity.Error); return false; }
        UpdateCoreFound();
        return true;
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        if (SaveAll()) Info("Saved. The recorder uses the new options the next time it is started.", InfoBarSeverity.Success);
    }

    private async void SaveRestart_Click(object sender, RoutedEventArgs e)
    {
        if (!SaveAll()) return;
        SaveRestartButton.IsEnabled = false;
        try
        {
            var err = await Actions.RestartRecorderAsync();
            if (err != null) Info(err, InfoBarSeverity.Error);
            else Info("Saved. The recorder was restarted with the new options.", InfoBarSeverity.Success);
        }
        finally { SaveRestartButton.IsEnabled = true; }
    }

    private void Reset_Click(object sender, RoutedEventArgs e)
    {
        _loading = true;
        try { ShowRecorderOptions(new RecorderOptions()); }
        finally { _loading = false; }
        Info("Defaults restored in the form. Choose Save to keep them.", InfoBarSeverity.Informational);
    }
}
