using Microsoft.Win32;

namespace WinFreezeTraceManager.Services;

/// <summary>"Start with Windows": a per-user Run entry that shows up in Settings > Apps > Startup and Task Manager.</summary>
internal static class StartupRegistration
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    private const string ValueName = "WinFreezeTrace Manager";
    private const string LegacyValueName = "WinFreezeTrace";   // 0.5.x: the recorder registered itself

    public static bool IsEnabled()
    {
        try
        {
            using var run = Registry.CurrentUser.OpenSubKey(RunKey);
            if (run?.GetValue(ValueName) is not string cmd || cmd.Length == 0) return false;
            return !DisabledByUser(ValueName);
        }
        catch { return false; }
    }

    /// <summary>Task Manager / Settings store "disabled" as a binary value whose first byte is odd.</summary>
    private static bool DisabledByUser(string name)
    {
        using var k = Registry.CurrentUser.OpenSubKey(ApprovedKey);
        return k?.GetValue(name) is byte[] data && data.Length > 0 && (data[0] & 1) != 0;
    }

    public static bool Set(bool enable, out string? error)
    {
        try
        {
            using var run = Registry.CurrentUser.CreateSubKey(RunKey);
            if (enable)
            {
                run.SetValue(ValueName, $"\"{AppPaths.ManagerExe}\" --background", RegistryValueKind.String);
                ClearApproved(ValueName);                    // re-enable if it had been switched off
            }
            else
            {
                run.DeleteValue(ValueName, throwOnMissingValue: false);
                ClearApproved(ValueName);
            }
            error = null;
            return true;
        }
        catch (Exception ex) { error = ex.Message; Log.Error("startup entry: " + ex.Message); return false; }
    }

    /// <summary>0.5.x let the recorder register itself. That entry would start the recorder without the Manager, with its own
    /// frozen options, so it is replaced by the Manager's entry (once).</summary>
    public static bool MigrateLegacy()
    {
        try
        {
            using var run = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
            if (run?.GetValue(LegacyValueName) is not string old || old.Length == 0) return false;
            bool wasEnabled = !DisabledByUser(LegacyValueName);
            run.DeleteValue(LegacyValueName, throwOnMissingValue: false);
            ClearApproved(LegacyValueName);
            if (wasEnabled) Set(true, out _);
            Log.Info("migrated the 0.5.x Start with Windows entry to the Manager");
            return true;
        }
        catch (Exception ex) { Log.Warn("legacy startup entry could not be migrated: " + ex.Message); return false; }
    }

    private static void ClearApproved(string name)
    {
        try
        {
            using var k = Registry.CurrentUser.OpenSubKey(ApprovedKey, writable: true);
            k?.DeleteValue(name, throwOnMissingValue: false);
        }
        catch { /* stale flag: ignore */ }
    }
}
