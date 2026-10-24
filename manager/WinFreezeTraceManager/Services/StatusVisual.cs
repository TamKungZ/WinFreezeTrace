using Windows.UI;

namespace WinFreezeTraceManager.Services;

internal enum RecorderPhase { Stopped, On, Off, Busy }

/// <summary>One place for how the recorder's state looks, so the dashboard, the navigation pane and the tray agree.</summary>
internal static class StatusVisual
{
    public static RecorderPhase PhaseOf(CoreStatus s) =>
        !s.Running ? RecorderPhase.Stopped :
        s.Capturing ? RecorderPhase.Busy :
        s.Paused ? RecorderPhase.Off :
        RecorderPhase.On;

    // Same colours as the tray icon badges: green = on, grey = off, orange = busy, red = recorder not running.
    public static Color ColorOf(RecorderPhase p) => p switch
    {
        RecorderPhase.On => Color.FromArgb(255, 16, 124, 16),
        RecorderPhase.Busy => Color.FromArgb(255, 232, 137, 12),
        RecorderPhase.Off => Color.FromArgb(255, 107, 111, 118),
        _ => Color.FromArgb(255, 196, 43, 28),
    };

    // Segoe Fluent Icons: CheckMark, Sync, Pause, Stop.
    public static string Glyph(RecorderPhase p) => p switch
    {
        RecorderPhase.On => "\uE73E",
        RecorderPhase.Busy => "\uE895",
        RecorderPhase.Off => "\uE769",
        _ => "\uE71A",
    };

    public static string ShortLabel(RecorderPhase p) => p switch
    {
        RecorderPhase.On => "Monitoring is on",
        RecorderPhase.Busy => "Saving a report",
        RecorderPhase.Off => "Monitoring is off",
        _ => "Recorder is not running",
    };
}
