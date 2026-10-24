namespace WinFreezeTraceManager.Services;

internal static class AppPaths
{
    /// <summary>%LOCALAPPDATA%\WinFreezeTrace. The recorder creates it with a private ACL (current user + SYSTEM),
    /// so the Manager never creates it just to write a log line; it only creates it when saving settings.</summary>
    public static string DataDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WinFreezeTrace");
    public static string DefaultReportsDir => Path.Combine(DataDir, "reports");
    public static string SettingsFile => Path.Combine(DataDir, "manager.json");
    public static string ManagerLog => Path.Combine(DataDir, "manager.log");
    public static string RecorderLog => Path.Combine(DataDir, "winfreezetrace.log");
    public static string AssetPath(string name) => Path.Combine(AppContext.BaseDirectory, "Assets", name);
    public static string ManagerExe => Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "WinFreezeTraceManager.exe");
}

internal static class Log
{
    private static readonly object Gate = new();

    public static void Write(string level, string message)
    {
        try
        {
            if (!Directory.Exists(AppPaths.DataDir)) return;
            lock (Gate)
            {
                var path = AppPaths.ManagerLog;
                var info = new FileInfo(path);
                if (info.Exists && info.Length > 256 * 1024)
                {
                    var old = path + ".old";
                    if (File.Exists(old)) File.Delete(old);
                    File.Move(path, old);
                }
                File.AppendAllText(path, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {level,-5} {message}{Environment.NewLine}");
            }
        }
        catch { /* logging must never break the app */ }
    }

    public static void Info(string message) => Write("INFO", message);
    public static void Warn(string message) => Write("WARN", message);
    public static void Error(string message) => Write("ERROR", message);
}
