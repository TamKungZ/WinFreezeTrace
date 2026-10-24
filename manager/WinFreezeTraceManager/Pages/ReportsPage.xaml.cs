using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using WinFreezeTraceManager.Services;

namespace WinFreezeTraceManager.Pages;

public sealed partial class ReportsPage : Page
{
    private string _root = "";

    public ReportsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => Reload();
    }

    private ReportItem? Selected => (List.SelectedItem as ListViewItem)?.Tag as ReportItem;

    private void Reload()
    {
        _root = Actions.ReportsDir();
        var keep = Selected?.Path;
        List.Items.Clear();
        ListViewItem? reselect = null;
        foreach (var r in ReportScanner.Scan(_root))
        {
            var panel = new StackPanel { Spacing = 2, Padding = new Thickness(0, 6, 0, 6) };
            panel.Children.Add(new TextBlock { Text = r.WhenText });
            panel.Children.Add(new TextBlock
            {
                Text = r.Trigger.Length > 0 ? r.Trigger : r.Name,
                FontSize = 12,
                Opacity = 0.7,
            });
            var item = new ListViewItem { Content = panel, Tag = r };
            List.Items.Add(item);
            if (keep != null && r.Path == keep) reselect = item;
        }
        EmptyText.Visibility = List.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (reselect != null) List.SelectedItem = reselect;
        else UpdateSelection();
    }

    private void UpdateSelection()
    {
        var r = Selected;
        OpenFolderButton.IsEnabled = r != null;
        OpenSummaryButton.IsEnabled = r != null && File.Exists(r.SummaryPath);
        DeleteButton.IsEnabled = r != null && ReportScanner.IsDeletable(_root, r.Path);
        Preview.Text = r == null ? "Select a report to preview its summary.txt." : ReportScanner.ReadSummary(r.SummaryPath);
    }

    private void List_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateSelection();
    private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();
    private void OpenRoot_Click(object sender, RoutedEventArgs e) => Actions.OpenFolder(_root.Length > 0 ? _root : Actions.ReportsDir());
    private void OpenFolder_Click(object sender, RoutedEventArgs e) { if (Selected is { } r) Actions.OpenFolder(r.Path); }
    private void OpenSummary_Click(object sender, RoutedEventArgs e) { if (Selected is { } r) Actions.OpenFile(r.SummaryPath); }

    private async void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } r || !ReportScanner.IsDeletable(_root, r.Path)) return;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Delete this report?",
            Content = r.Name + "\n\nThe folder and everything in it (including target.dmp, if any) is deleted permanently.",
            PrimaryButtonText = "Delete",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        try { Directory.Delete(r.Path, recursive: true); Log.Info("deleted report " + r.Name); }
        catch (Exception ex)
        {
            Log.Warn("could not delete " + r.Path + ": " + ex.Message);
            Preview.Text = "Could not delete the report: " + ex.Message;
            return;
        }
        Reload();
    }
}
