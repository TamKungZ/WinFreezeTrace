using System.Globalization;
using System.Text;

namespace WinFreezeTraceManager.Services;

internal sealed class ReportItem
{
    public string Path { get; init; } = "";
    public string Name { get; init; } = "";
    public DateTime? When { get; init; }
    public string Trigger { get; init; } = "";
    public string SummaryPath => System.IO.Path.Combine(Path, "summary.txt");
    public string WhenText => When?.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.CurrentCulture) ?? Name;

    private static readonly (string Key, string Label)[] TriggerLabels =
    {
        ("scheduler_stall", "System stall"),
        ("storage_stall", "Storage stall"),
        ("foreground_app_hang", "App not responding"),
        ("manual_hotkey", "Manual capture"),
    };

    /// <summary>The trigger folder suffix (scheduler_stall, foreground_app_hang, ...) in words a person can read.</summary>
    public string TriggerLabel
    {
        get
        {
            foreach (var (key, label) in TriggerLabels)
                if (Trigger.StartsWith(key, StringComparison.Ordinal)) return label;
            return Trigger.Length == 0 ? "Report" : Trigger.Replace('_', ' ');
        }
    }

    /// <summary>"5 min ago", "yesterday", ... Empty when the folder name has no usable time.</summary>
    public string AgoText
    {
        get
        {
            if (When is not { } w) return "";
            var d = DateTime.Now - w;
            if (d < TimeSpan.Zero) return "";
            if (d.TotalMinutes < 1) return "just now";
            if (d.TotalHours < 1) return (int)d.TotalMinutes + " min ago";
            if (d.TotalDays < 1) return (int)d.TotalHours + " h ago";
            if (d.TotalDays < 2) return "yesterday";
            return (int)d.TotalDays + " days ago";
        }
    }
}

/// <summary>Lists incident folders named yyyyMMdd_HHmmss_&lt;trigger&gt;[_n] under the report root.</summary>
internal static class ReportScanner
{
    public static List<ReportItem> Scan(string root)
    {
        var items = new List<ReportItem>();
        try
        {
            if (!Directory.Exists(root)) return items;
            foreach (var dir in Directory.EnumerateDirectories(root))
            {
                var name = System.IO.Path.GetFileName(dir);
                if (!LooksLikeIncident(name)) continue;
                DateTime? when = DateTime.TryParseExact(name.AsSpan(0, 15), "yyyyMMdd_HHmmss", CultureInfo.InvariantCulture,
                                                        DateTimeStyles.None, out var t) ? t : null;
                items.Add(new ReportItem { Path = dir, Name = name, When = when, Trigger = name.Length > 16 ? name.Substring(16) : "" });
            }
        }
        catch (Exception ex) { Log.Warn("report scan failed: " + ex.Message); }
        items.Sort((a, b) => string.CompareOrdinal(b.Name, a.Name));
        return items;
    }

    public static bool LooksLikeIncident(string name)
    {
        if (name.Length < 17 || name[8] != '_' || name[15] != '_') return false;
        for (int i = 0; i < 15; i++) if (i != 8 && !char.IsAsciiDigit(name[i])) return false;
        return true;
    }

    /// <summary>Only folders directly under the report root with an incident-style name may be deleted.</summary>
    public static bool IsDeletable(string root, string path)
    {
        try
        {
            var r = System.IO.Path.GetFullPath(root).TrimEnd('\\');
            var p = System.IO.Path.GetFullPath(path).TrimEnd('\\');
            var parent = System.IO.Path.GetDirectoryName(p)?.TrimEnd('\\');
            return string.Equals(parent, r, StringComparison.OrdinalIgnoreCase) && LooksLikeIncident(System.IO.Path.GetFileName(p));
        }
        catch { return false; }
    }

    public static string ReadSummary(string summaryPath, int maxBytes = 64 * 1024)
    {
        try
        {
            using var fs = new FileStream(summaryPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var buf = new byte[(int)Math.Min(fs.Length, maxBytes)];
            int n = fs.Read(buf, 0, buf.Length);
            var text = Encoding.UTF8.GetString(buf, 0, n);
            return fs.Length > maxBytes ? text + "\r\n\r\n[... truncated, open summary.txt for the rest ...]" : text;
        }
        catch (FileNotFoundException) { return "summary.txt is not there (yet): the report may still be being written, or it was removed."; }
        catch (Exception ex) { return "Could not read summary.txt: " + ex.Message; }
    }
}
