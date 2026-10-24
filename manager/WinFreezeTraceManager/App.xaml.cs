using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager;

public partial class App : Application
{
    private const string SingletonName = @"Local\WinFreezeTrace.Manager.Singleton.v1";
    private const string ShowEventName = @"Local\WinFreezeTrace.Manager.Show.v1";
    private const string QuitEventName = @"Local\WinFreezeTrace.Manager.Quit.v1";

    internal static ManagerSettings Settings { get; private set; } = new();
    internal static MonitorService Monitor { get; private set; } = null!;
    internal static TrayIcon? Tray { get; private set; }
    internal static MainWindow? MainWindowInstance { get; private set; }
    internal static bool Exiting { get; private set; }
    internal static IntPtr WindowHandle => MainWindowInstance?.Handle ?? IntPtr.Zero;

    private Mutex? _mutex;                       // held for the life of the process: it is the single-instance lock
    private EventWaitHandle? _showEvent, _quitEvent;
    private DispatcherQueue? _queue;
    private string _lastReport = "";

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) => { Log.Error("unhandled exception: " + e.Exception); e.Handled = true; };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var argv = Environment.GetCommandLineArgs().Skip(1).ToArray();
        bool background = argv.Any(a => a.Equals("--background", StringComparison.OrdinalIgnoreCase));
        bool quit = argv.Any(a => a.Equals("--exit", StringComparison.OrdinalIgnoreCase));

        _mutex = new Mutex(true, SingletonName, out bool first);
        _showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName);
        _quitEvent = new EventWaitHandle(false, EventResetMode.AutoReset, QuitEventName);
        if (!first || quit)
        {
            if (!first) { if (quit) _quitEvent.Set(); else if (!background) _showEvent.Set(); }
            Environment.Exit(0);                 // a second copy never shows anything: it asks the first one and leaves
            return;
        }

        _queue = DispatcherQueue.GetForCurrentThread();
        Settings = ManagerSettings.Load();
        Monitor = new MonitorService(_queue);
        if (StartupRegistration.MigrateLegacy()) Settings.Save(out _);

        MainWindowInstance = new MainWindow();

        Tray = new TrayIcon { MenuModelProvider = BuildMenuModel };
        Tray.OpenRequested += ShowMainWindow;
        Tray.ToggleRequested += () => RunAction(Actions.ToggleMonitoringAsync);
        Tray.CaptureRequested += () => RunAction(Actions.CaptureNowAsync);
        Tray.OpenReportsRequested += () => Actions.OpenFolder(Actions.ReportsDir());
        Tray.ToggleStartupRequested += ToggleStartup;
        Tray.ExitRequested += () => ExitApp(Settings.StopRecorderOnExit);
        Tray.ExitAndStopRequested += () => ExitApp(true);
        Tray.BalloonClicked += OpenLastReport;

        Monitor.StatusChanged += UpdateTray;
        Monitor.CaptureSaved += OnCaptureSaved;
        Monitor.CaptureFailed += () => Tray?.ShowBalloon("WinFreezeTrace could not save a report", "See winfreezetrace.log in the WinFreezeTrace folder for details.", true);
        Monitor.RecorderLost += () => Tray?.ShowBalloon("The recorder stopped", "WinFreezeTrace is no longer recording. Open the Manager to start it again.", true);
        UpdateTray(CoreStatus.NotRunning);

        ThreadPool.RegisterWaitForSingleObject(_showEvent, (_, _) => _queue.TryEnqueue(ShowMainWindow), null, -1, false);
        ThreadPool.RegisterWaitForSingleObject(_quitEvent, (_, _) => _queue.TryEnqueue(() => ExitApp(false)), null, -1, false);

        Monitor.Start();
        if (!background) ShowMainWindow();
        StartRecorderIfWanted();
        Log.Info("Manager started" + (background ? " in the background" : ""));
    }

    private async void StartRecorderIfWanted()
    {
        if (!Settings.AutoStartRecorder) return;
        try
        {
            if ((await CoreClient.SendAsync("STATUS")).Running) return;
            var err = await Actions.StartRecorderAsync();
            if (err != null) Tray?.ShowBalloon("WinFreezeTrace", err, true);
        }
        catch (Exception ex) { Log.Error("auto-start failed: " + ex.Message); }
    }

    internal static void ShowMainWindow() => MainWindowInstance?.Present();

    /// <summary>Runs a recorder command from the tray and reports a failure as a balloon.</summary>
    private static async void RunAction(Func<Task<string?>> action)
    {
        try
        {
            var err = await action();
            if (err != null) Tray?.ShowBalloon("WinFreezeTrace", err, true);
        }
        catch (Exception ex) { Log.Error("tray action failed: " + ex); }
    }

    private static TrayMenuModel BuildMenuModel()
    {
        var s = Monitor.Current;
        string state = !s.Running ? "recorder not running" : s.Capturing ? "writing a report" : s.Paused ? "monitoring OFF" : "monitoring ON";
        return new TrayMenuModel
        {
            Header = "WinFreezeTrace " + (s.Version.Length > 0 ? s.Version + " - " : "- ") + state,
            RecorderRunning = s.Running,
            Paused = s.Paused,
            Capturing = s.Capturing,
            StartupEnabled = StartupRegistration.IsEnabled(),
        };
    }

    private void UpdateTray(CoreStatus s)
    {
        if (Tray == null) return;
        string v = s.Version.Length > 0 ? " " + s.Version : "";
        if (!s.Running) Tray.SetState(TrayState.Stopped, "WinFreezeTrace - recorder not running (click to open the Manager)");
        else if (s.Capturing) Tray.SetState(TrayState.Busy, "WinFreezeTrace" + v + " - writing a report...");
        else if (s.Paused) Tray.SetState(TrayState.Off, "WinFreezeTrace" + v + " - monitoring OFF");
        else Tray.SetState(TrayState.On, "WinFreezeTrace" + v + " - monitoring ON");
    }

    private void OnCaptureSaved(string folder)
    {
        _lastReport = folder;
        if (Settings.ShowNotifications)
            Tray?.ShowBalloon("Freeze report saved", "Click here to open the report folder. Start with summary.txt.", false);
    }

    private void OpenLastReport()
    {
        if (_lastReport.Length > 0 && Directory.Exists(_lastReport)) Actions.OpenFolder(_lastReport);
        else Actions.OpenFolder(Actions.ReportsDir());
    }

    private void ToggleStartup()
    {
        bool want = !StartupRegistration.IsEnabled();
        bool ok = StartupRegistration.Set(want, out var err);
        Tray?.ShowBalloon("WinFreezeTrace Manager",
            ok ? (want ? "The Manager will now start when you sign in to Windows." : "The Manager will no longer start with Windows.")
               : "Could not change the Windows Startup setting: " + err, !ok);
    }

    /// <summary>Quits the Manager. The recorder keeps running unless stopRecorder is true.</summary>
    internal static async void ExitApp(bool stopRecorder)
    {
        if (Exiting) return;
        Exiting = true;
        try
        {
            if (stopRecorder) await CoreLauncher.StopAsync();
            Monitor.Stop();
            Tray?.Dispose();
            Tray = null;
            Log.Info("Manager exiting" + (stopRecorder ? " (recorder stopped)" : ""));
            MainWindowInstance?.Close();
            Current.Exit();
        }
        catch (Exception ex)
        {
            Log.Error("exit failed: " + ex.Message);
            Environment.Exit(0);
        }
    }
}
