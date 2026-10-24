using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager.Pages;

public sealed partial class DashboardPage : Page
{
    private bool _acting;
    private int _knownCaptures = -1;
    private ReportItem? _last;

    public DashboardPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            App.Monitor.StatusChanged += OnStatus;
            Apply(App.Monitor.Current);
            RefreshLastReport();
        };
        Unloaded += (_, _) => App.Monitor.StatusChanged -= OnStatus;
    }

    private void OnStatus(CoreStatus s) => Apply(s);

    private void Apply(CoreStatus s)
    {
        var phase = StatusVisual.PhaseOf(s);
        StatusBadge.Background = new SolidColorBrush(StatusVisual.ColorOf(phase));
        StatusGlyph.Glyph = StatusVisual.Glyph(phase);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(StatusBadge, StatusVisual.ShortLabel(phase));

        switch (phase)
        {
            case RecorderPhase.Stopped:
                StatusTitle.Text = "The recorder is not running";
                StatusDetail.Text = "Nothing is being watched, so a freeze will not be captured. Start the recorder to protect this PC.";
                PowerButton.Content = "Start the recorder";
                break;
            case RecorderPhase.Busy:
                StatusTitle.Text = "Saving a report...";
                StatusDetail.Text = "A freeze was detected (or you asked for a capture). This can take several seconds; the report appears below when it is done.";
                PowerButton.Content = s.Paused ? "Turn monitoring on" : "Turn monitoring off";
                break;
            case RecorderPhase.Off:
                StatusTitle.Text = "Monitoring is off";
                StatusDetail.Text = "Nothing is watched or captured, and the hotkey is disabled. Recent history was discarded.";
                PowerButton.Content = "Turn monitoring on";
                break;
            default:
                StatusTitle.Text = "Monitoring is on";
                StatusDetail.Text = "WinFreezeTrace " + s.Version + " is watching for system stalls and unresponsive apps. "
                                  + "When one happens, a report is saved automatically.";
                PowerButton.Content = "Turn monitoring off";
                break;
        }

        PowerButton.IsEnabled = !_acting;
        CaptureButton.IsEnabled = !_acting && s.Running && !s.Paused && !s.Capturing;
        CaptureHint.Text = s.Running && s.Paused ? "Turn monitoring on to capture a report." : "";
        MoreButton.Visibility = s.Running ? Visibility.Visible : Visibility.Collapsed;
        MoreButton.IsEnabled = !_acting;

        CoreMissing.IsOpen = !s.Running && CoreLauncher.FindExe(App.Settings) == null;
        FailureBar.IsOpen = s.Running && s.Failures > 0;
        if (FailureBar.IsOpen)
            FailureBar.Message = s.Failures + (s.Failures == 1 ? " report" : " reports")
                               + " could not be written (for example because the disk is full or the folder is not writable). The recorder log has the reason.";

        SavedCount.Text = s.Running ? s.Captures.ToString() : "-";
        FailedCount.Text = s.Running ? s.Failures.ToString() : "-";
        if (!s.Running) { HotkeyState.Text = "-"; HotkeyHint.Text = "available while the recorder runs"; }
        else if (s.Hotkey) { HotkeyState.Text = "Ready"; HotkeyHint.Text = "captures a report on demand"; }
        else { HotkeyState.Text = "Not active"; HotkeyHint.Text = "disabled, or another program owns it"; }

        if (s.Running && s.Captures != _knownCaptures)
        {
            _knownCaptures = s.Captures;
            RefreshLastReport();
        }
    }

    private void RefreshLastReport()
    {
        var items = ReportScanner.Scan(Actions.ReportsDir());
        _last = items.Count > 0 ? items[0] : null;
        if (_last == null)
        {
            LastTitle.Text = "No reports yet";
            LastPath.Text = "Reports are saved in " + Actions.ReportsDir();
            LastHint.Visibility = Visibility.Visible;
        }
        else
        {
            LastTitle.Text = _last.TriggerLabel + "  -  " + _last.WhenText + (_last.AgoText.Length > 0 ? " (" + _last.AgoText + ")" : "");
            LastPath.Text = _last.Path;
            LastHint.Visibility = Visibility.Collapsed;
        }
        OpenLastFolderButton.IsEnabled = _last != null;
        OpenLastSummaryButton.IsEnabled = _last != null && File.Exists(_last.SummaryPath);
    }

    private async Task RunAsync(Func<Task<string?>> action)
    {
        if (_acting) return;
        _acting = true;
        Apply(App.Monitor.Current);
        try
        {
            var err = await action();
            Notice.Message = err ?? "";
            Notice.IsOpen = err != null;
        }
        catch (Exception ex) { Notice.Message = ex.Message; Notice.IsOpen = true; Log.Error("dashboard action: " + ex); }
        finally { _acting = false; Apply(App.Monitor.Current); }
    }

    private async void Power_Click(object sender, RoutedEventArgs e) => await RunAsync(Actions.ToggleMonitoringAsync);
    private async void Capture_Click(object sender, RoutedEventArgs e) => await RunAsync(Actions.CaptureNowAsync);

    private async void Stop_Click(object sender, RoutedEventArgs e)
    {
        // Stopping is the one action here that removes protection, so ask first.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Stop the recorder?",
            Content = "While it is stopped, freezes are not captured. You can start it again from this page or from the tray icon.",
            PrimaryButtonText = "Stop the recorder",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        await RunAsync(Actions.StopRecorderAsync);
    }

    private void OpenLastFolder_Click(object sender, RoutedEventArgs e) { if (_last != null) Actions.OpenFolder(_last.Path); }
    private void OpenLastSummary_Click(object sender, RoutedEventArgs e) { if (_last != null) Actions.OpenFile(_last.SummaryPath); }
    private void AllReports_Click(object sender, RoutedEventArgs e) => App.MainWindowInstance?.GoToReports();
    private void OpenSettings_Click(object sender, RoutedEventArgs e) => App.MainWindowInstance?.GoToSettings();
    private void OpenRecorderLog_Click(object sender, RoutedEventArgs e) => Actions.OpenFile(AppPaths.RecorderLog);
}
