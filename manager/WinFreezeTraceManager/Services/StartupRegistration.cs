using System.Runtime.InteropServices;
using Microsoft.Win32;
using Windows.ApplicationModel;

namespace WinFreezeTraceManager.Services;

/// <summary>"Start with Windows": a per-user Run entry that shows up in Settings > Apps > Startup and Task Manager.
/// In the MSIX package a Run entry would be written to the package's private registry hive and never run, so the
/// package's manifest StartupTask is used instead (same place in Settings > Apps > Startup).</summary>
internal static class StartupRegistration
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    private const string ValueName = "WinFreezeTrace Manager";
    private const string LegacyValueName = "WinFreezeTrace";   // 0.5.x: the recorder registered itself

    private const string StartupTaskId = "ManagerStartup";   // AppxManifest.xml

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetCurrentPackageFullName(ref uint length, char[]? name);

    /// <summary>True when running from the MSIX package (the NSIS install and the portable zip have no package identity).</summary>
    public static bool IsPackaged { get; } = DetectPackaged();

    private static bool DetectPackaged()
    {
        try { uint len = 0; return GetCurrentPackageFullName(ref len, null) == 122; }   // ERROR_INSUFFICIENT_BUFFER: there is a package
        catch { return false; }
    }

    public static bool IsEnabled()
    {
        if (IsPackaged) return Task.Run(IsTaskEnabled).GetAwaiter().GetResult();
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

    private static async Task<bool> IsTaskEnabled()
    {
        try
        {
            var task = await StartupTask.GetAsync(StartupTaskId);
            return task.State is StartupTaskState.Enabled or StartupTaskState.EnabledByPolicy;
        }
        catch { return false; }
    }

    private static async Task<string?> SetTask(bool enable)
    {
        var task = await StartupTask.GetAsync(StartupTaskId);
        if (!enable) { task.Disable(); return null; }
        var state = task.State;
        if (state is StartupTaskState.Disabled) state = await task.RequestEnableAsync();
        return state switch
        {
            StartupTaskState.Enabled or StartupTaskState.EnabledByPolicy => null,
            StartupTaskState.DisabledByUser => "Start with Windows was switched off in Settings > Apps > Startup. Switch it on there.",
            StartupTaskState.DisabledByPolicy => "Start with Windows is disabled by policy.",
            _ => "Windows did not enable the startup task.",
        };
    }

    public static bool Set(bool enable, out string? error)
    {
        if (IsPackaged)
        {
            try { error = Task.Run(() => SetTask(enable)).GetAwaiter().GetResult(); }
            catch (Exception ex) { error = ex.Message; }
            if (error != null) Log.Error("startup task: " + error);
            return error == null;
        }
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
        if (IsPackaged) return false;
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
