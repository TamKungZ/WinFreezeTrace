using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
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
            panel.Children.Add(new TextBlock { Text = r.TriggerLabel });
            panel.Children.Add(new TextBlock
            {
                Text = r.WhenText + (r.AgoText.Length > 0 ? "  -  " + r.AgoText : ""),
                FontSize = 12,
                Opacity = 0.7,
            });
            var item = new ListViewItem { Content = panel, Tag = r };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(item, r.TriggerLabel + ", " + r.WhenText);
            List.Items.Add(item);
            if (keep != null && r.Path == keep) reselect = item;
        }
        int n = List.Items.Count;
        Subtitle.Text = (n == 0 ? "No reports" : n == 1 ? "1 report" : n + " reports") + " in " + _root;
        EmptyPanel.Visibility = n == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (reselect != null) List.SelectedItem = reselect;
        else UpdateSelection();
    }

    private void UpdateSelection()
    {
        var r = Selected;
        OpenFolderButton.IsEnabled = r != null;
        OpenSummaryButton.IsEnabled = r != null && File.Exists(r.SummaryPath);
        DeleteButton.IsEnabled = r != null && ReportScanner.IsDeletable(_root, r.Path);
        ShowPreview(r == null ? null : ReportScanner.ReadSummary(r.SummaryPath), r);
    }

    /// <summary>text == null shows the "select a report" hint instead of a preview.</summary>
    private void ShowPreview(string? text, ReportItem? r)
    {
        bool has = text != null;
        Preview.Text = text ?? "";
        Preview.Visibility = has ? Visibility.Visible : Visibility.Collapsed;
        PreviewHint.Visibility = has ? Visibility.Collapsed : Visibility.Visible;
        PreviewTitle.Visibility = has ? Visibility.Visible : Visibility.Collapsed;
        PreviewTitle.Text = r == null ? "" : r.TriggerLabel + "  -  " + r.WhenText;
    }

    private void List_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateSelection();
    private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();
    private void OpenRoot_Click(object sender, RoutedEventArgs e) => Actions.OpenFolder(_root.Length > 0 ? _root : Actions.ReportsDir());
    private void OpenFolder_Click(object sender, RoutedEventArgs e) { if (Selected is { } r) Actions.OpenFolder(r.Path); }
    private void OpenSummary_Click(object sender, RoutedEventArgs e) { if (Selected is { } r) Actions.OpenFile(r.SummaryPath); }

    private void List_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == Windows.System.VirtualKey.Delete && DeleteButton.IsEnabled)
        {
            e.Handled = true;
            Delete_Click(sender, e);
        }
    }

    private async void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } r || !ReportScanner.IsDeletable(_root, r.Path)) return;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Delete this report?",
            Content = r.TriggerLabel + " - " + r.WhenText + "\n" + r.Name
                    + "\n\nThe folder and everything in it (including target.dmp, if any) is deleted permanently.",
            PrimaryButtonText = "Delete",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        try { Directory.Delete(r.Path, recursive: true); Log.Info("deleted report " + r.Name); }
        catch (Exception ex)
        {
            Log.Warn("could not delete " + r.Path + ": " + ex.Message);
            ShowPreview("Could not delete the report:\n" + ex.Message, r);
            return;
        }
        Reload();
    }
}
