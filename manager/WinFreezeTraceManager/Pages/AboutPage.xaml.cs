using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager.Pages;

public sealed partial class AboutPage : Page
{
    public AboutPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            try { Logo.Source = new BitmapImage(new Uri(AppPaths.AssetPath("logo.png"))); } catch (Exception ex) { Log.Warn("logo: " + ex.Message); }
            var v = typeof(App).Assembly.GetName().Version;
            ManagerVersion.Text = "Manager version " + (v == null ? "?" : v.ToString(3));
            var s = App.Monitor.Current;
            RecorderVersion.Text = s.Running ? "Recorder version " + s.Version : "Recorder: not running";
        };
    }

    private void RecorderLog_Click(object sender, RoutedEventArgs e) => Actions.OpenFile(AppPaths.RecorderLog);
    private void ManagerLog_Click(object sender, RoutedEventArgs e) => Actions.OpenFile(AppPaths.ManagerLog);
    private void DataFolder_Click(object sender, RoutedEventArgs e) => Actions.OpenFolder(AppPaths.DataDir);
    private void Project_Click(object sender, RoutedEventArgs e) => Actions.OpenUrl("https://github.com/TamKungZ/WinFreezeTrace");
}
