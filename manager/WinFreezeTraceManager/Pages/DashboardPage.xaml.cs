using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
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
        Color dot;
        if (!s.Running)
        {
            dot = Color.FromArgb(255, 150, 150, 150);
            StatusTitle.Text = "The recorder is not running";
            StatusDetail.Text = "Nothing is being recorded. Start the recorder to watch for freezes.";
            PowerButton.Content = "Start the recorder";
        }
        else if (s.Capturing)
        {
            dot = Color.FromArgb(255, 232, 137, 12);
            StatusTitle.Text = "Writing a report...";
            StatusDetail.Text = "A freeze was detected (or a capture was requested). This can take several seconds.";
            PowerButton.Content = s.Paused ? "Turn monitoring on" : "Turn monitoring off";
        }
        else if (s.Paused)
        {
            dot = Color.FromArgb(255, 122, 128, 138);
            StatusTitle.Text = "Monitoring is off";
            StatusDetail.Text = "No sampling, no probing, no captures and no hotkey. The in-memory history is discarded.";
            PowerButton.Content = "Turn monitoring on";
        }
        else
        {
            dot = Color.FromArgb(255, 46, 160, 80);
            StatusTitle.Text = "Monitoring is on";
            StatusDetail.Text = "WinFreezeTrace " + s.Version + " (process " + s.Pid + ") is watching for stalls and unresponsive windows.";
            PowerButton.Content = "Turn monitoring off";
        }
        StatusDot.Fill = new SolidColorBrush(dot);

        PowerButton.IsEnabled = !_acting;
        CaptureButton.IsEnabled = !_acting && s.Running && !s.Paused && !s.Capturing;
        StopButton.Visibility = s.Running ? Visibility.Visible : Visibility.Collapsed;
        StopButton.IsEnabled = !_acting;

        Counters.Text = s.Running
            ? "Reports saved: " + s.Captures + "   Failed: " + s.Failures
            : "Counters are available while the recorder runs.";
        HotkeyText.Text = !s.Running ? "" : s.Hotkey ? "Ctrl+Alt+F12 forces a capture while monitoring is on."
                                                     : "The Ctrl+Alt+F12 hotkey is not active (disabled, or another program owns it).";

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
            LastTitle.Text = "No report yet.";
            LastPath.Text = Actions.ReportsDir();
        }
        else
        {
            LastTitle.Text = _last.WhenText + "  -  " + _last.Trigger;
            LastPath.Text = _last.Path;
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
    private async void Stop_Click(object sender, RoutedEventArgs e) => await RunAsync(Actions.StopRecorderAsync);

    private void OpenLastFolder_Click(object sender, RoutedEventArgs e) { if (_last != null) Actions.OpenFolder(_last.Path); }
    private void OpenLastSummary_Click(object sender, RoutedEventArgs e) { if (_last != null) Actions.OpenFile(_last.SummaryPath); }
    private void AllReports_Click(object sender, RoutedEventArgs e) => App.MainWindowInstance?.GoToReports();
}
