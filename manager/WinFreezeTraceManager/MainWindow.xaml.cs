using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using WinFreezeTraceManager.Pages;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager;

public sealed partial class MainWindow : Window
{
    private const int SW_HIDE = 0, SW_RESTORE = 9;
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hWnd);

    internal IntPtr Handle { get; }

    public MainWindow()
    {
        InitializeComponent();
        Title = "WinFreezeTrace Manager";
        Handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        SizeAndCenter();
        try { AppWindow.SetIcon(AppPaths.AssetPath("app.ico")); } catch (Exception ex) { Log.Warn("window icon: " + ex.Message); }
        try { TitleIcon.Source = new BitmapImage(new Uri(AppPaths.AssetPath("logo.png"))); } catch (Exception ex) { Log.Warn("title icon: " + ex.Message); }

        // Title bar: our own row, caption buttons transparent so the window material shows through them.
        try
        {
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(AppTitleBar);
            var tb = AppWindow.TitleBar;
            tb.ButtonBackgroundColor = Colors.Transparent;
            tb.ButtonInactiveBackgroundColor = Colors.Transparent;
        }
        catch (Exception ex) { Log.Warn("title bar: " + ex.Message); }

        ApplyBackdrop(App.Settings.Backdrop);
        AppWindow.Closing += OnClosing;

        App.Monitor.StatusChanged += UpdatePaneStatus;
        UpdatePaneStatus(App.Monitor.Current);

        Nav.SelectedItem = Nav.MenuItems[0];
    }

    /// <summary>DPI-aware start size that always fits the screen, centred on the work area.</summary>
    private void SizeAndCenter()
    {
        try
        {
            double scale = Math.Max(1.0, GetDpiForWindow(Handle) / 96.0);
            var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest).WorkArea;
            int margin = (int)(48 * scale);
            int w = Math.Max((int)(640 * scale), Math.Min((int)(1040 * scale), area.Width - margin));
            int h = Math.Max((int)(480 * scale), Math.Min((int)(760 * scale), area.Height - margin));
            AppWindow.Resize(new Windows.Graphics.SizeInt32(w, h));
            AppWindow.Move(new Windows.Graphics.PointInt32(area.X + (area.Width - w) / 2, area.Y + (area.Height - h) / 2));
        }
        catch (Exception ex) { Log.Warn("window size: " + ex.Message); }
    }

    /// <summary>kind: acrylic (blurred, see-through), mica, micaalt or none (solid). Falls back when the OS lacks the material.</summary>
    internal void ApplyBackdrop(string kind)
    {
        SystemBackdrop? backdrop = null;
        try
        {
            switch (kind)
            {
                case "none":
                    break;
                case "mica":
                    if (MicaController.IsSupported()) backdrop = new MicaBackdrop();
                    break;
                case "micaalt":
                    if (MicaController.IsSupported()) backdrop = new MicaBackdrop { Kind = MicaKind.BaseAlt };
                    break;
                default:
                    if (DesktopAcrylicController.IsSupported()) backdrop = new DesktopAcrylicBackdrop();
                    else if (MicaController.IsSupported()) backdrop = new MicaBackdrop();
                    break;
            }
            SystemBackdrop = backdrop;
        }
        catch (Exception ex)
        {
            Log.Warn("backdrop: " + ex.Message);
            backdrop = null;
        }
        // No material available (or "Solid" chosen): paint an opaque background instead of a see-through hole.
        SolidLayer.Visibility = backdrop == null ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdatePaneStatus(CoreStatus s)
    {
        var phase = StatusVisual.PhaseOf(s);
        PaneDot.Fill = new SolidColorBrush(StatusVisual.ColorOf(phase));
        PaneStatusText.Text = StatusVisual.ShortLabel(phase);
    }

    /// <summary>Show the window and bring it to the front (also used to restore it from the tray).</summary>
    internal void Present()
    {
        ShowWindow(Handle, SW_RESTORE);
        Activate();
        SetForegroundWindow(Handle);
    }

    internal void GoToReports() => Nav.SelectedItem = Nav.MenuItems[1];
    internal void GoToSettings() => Nav.SelectedItem = Nav.SettingsItem;

    private void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (App.Exiting) return;                    // a real exit: let the window close
        if (App.Settings.CloseToTray && App.Tray != null && App.Tray.IsVisible)
        {
            args.Cancel = true;                     // keep running in the tray
            ShowWindow(Handle, SW_HIDE);
            if (!App.Settings.TrayHintShown)        // once: otherwise the window just seems to vanish
            {
                App.Settings.TrayHintShown = true;
                App.Settings.Save(out _);
                App.Tray.ShowBalloon("WinFreezeTrace is still running",
                    "The Manager moved to the notification area. Click its icon to open it again, or use the icon's menu to exit.", false);
            }
            return;
        }
        args.Cancel = true;                         // no tray icon to come back to, so closing the window quits the Manager
        App.ExitApp(App.Settings.StopRecorderOnExit);
    }

    private void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        Type page;
        if (args.IsSettingsSelected) page = typeof(SettingsPage);
        else
        {
            switch (args.SelectedItemContainer?.Tag as string)
            {
                case "reports": page = typeof(ReportsPage); break;
                case "about": page = typeof(AboutPage); break;
                default: page = typeof(DashboardPage); break;
            }
        }
        if (ContentFrame.CurrentSourcePageType != page) ContentFrame.Navigate(page);
    }
}
