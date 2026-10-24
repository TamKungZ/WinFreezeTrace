using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using WinFreezeTraceManager.Pages;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager;

public sealed partial class MainWindow : Window
{
    private const int SW_HIDE = 0, SW_RESTORE = 9;
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hWnd);

    internal IntPtr Handle { get; }

    public MainWindow()
    {
        InitializeComponent();
        Title = "WinFreezeTrace Manager";
        Handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1040, 760));
        try { AppWindow.SetIcon(AppPaths.AssetPath("app.ico")); } catch (Exception ex) { Log.Warn("window icon: " + ex.Message); }
        try { SystemBackdrop = new MicaBackdrop(); } catch (Exception ex) { Log.Warn("backdrop: " + ex.Message); }
        AppWindow.Closing += OnClosing;
        Nav.SelectedItem = Nav.MenuItems[0];
    }

    /// <summary>Show the window and bring it to the front (also used to restore it from the tray).</summary>
    internal void Present()
    {
        ShowWindow(Handle, SW_RESTORE);
        Activate();
        SetForegroundWindow(Handle);
    }

    internal void GoToReports() => Nav.SelectedItem = Nav.MenuItems[1];

    private void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (App.Exiting) return;                    // a real exit: let the window close
        if (App.Settings.CloseToTray && App.Tray != null && App.Tray.IsVisible)
        {
            args.Cancel = true;                     // keep running in the tray
            ShowWindow(Handle, SW_HIDE);
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
