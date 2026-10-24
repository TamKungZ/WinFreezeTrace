using System.Runtime.InteropServices;

namespace WinFreezeTraceManager.Services;

internal enum TrayState { On, Off, Busy, Stopped }

/// <summary>What the context menu should show; filled by the owner right before the menu opens.</summary>
internal sealed class TrayMenuModel
{
    public string Header { get; set; } = "WinFreezeTrace Manager";
    public bool RecorderRunning { get; set; }
    public bool Paused { get; set; }
    public bool Capturing { get; set; }
    public bool StartupEnabled { get; set; }
}

/// <summary>
/// Notification-area icon with a native popup menu, driven by Shell_NotifyIcon. WinUI 3 has no tray API, and a hidden top-level
/// window on the UI thread (the XAML message loop dispatches to it) is the least fragile way to receive the callbacks. It must
/// be a normal top-level window, not message-only, to receive the TaskbarCreated broadcast after Explorer restarts.
/// </summary>
internal sealed class TrayIcon : IDisposable
{
    // ---- Win32 ----
    private const uint WM_NULL = 0x0000, WM_DESTROY = 0x0002, WM_CONTEXTMENU = 0x007B, WM_APP = 0x8000, WM_USER = 0x0400;
    private const uint WM_TRAY = WM_APP + 1;
    private const uint NIN_SELECT = WM_USER + 0, NIN_KEYSELECT = WM_USER + 1, NIN_BALLOONUSERCLICK = WM_USER + 5;
    private const uint NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2, NIM_SETVERSION = 4;
    private const uint NIF_MESSAGE = 0x1, NIF_ICON = 0x2, NIF_TIP = 0x4, NIF_INFO = 0x10, NIF_SHOWTIP = 0x80;
    private const uint NIIF_INFO = 0x1, NIIF_WARNING = 0x2, NIIF_NOSOUND = 0x10;
    private const uint NOTIFYICON_VERSION_4 = 4;
    private const uint MF_STRING = 0x0, MF_GRAYED = 0x1, MF_DISABLED = 0x2, MF_CHECKED = 0x8, MF_SEPARATOR = 0x800;
    private const uint TPM_RIGHTBUTTON = 0x2, TPM_BOTTOMALIGN = 0x20, TPM_RETURNCMD = 0x100, TPM_RIGHTALIGN = 0x8;
    private const int SM_CXSMICON = 49, SM_MENUDROPALIGNMENT = 40;
    private const uint IMAGE_ICON = 1, LR_LOADFROMFILE = 0x10;
    private const uint WS_POPUP = 0x80000000, WS_EX_TOOLWINDOW = 0x80;

    private const int ID_OPEN = 1, ID_TOGGLE = 2, ID_CAPTURE = 3, ID_FOLDER = 4, ID_STARTUP = 5, ID_EXIT = 6, ID_EXIT_STOP = 7;

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEX
    {
        public uint cbSize, style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
        public IntPtr hIconSm;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NOTIFYICONDATA
    {
        public uint cbSize;
        public IntPtr hWnd;
        public uint uID, uFlags, uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState, dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags;
        public Guid guidItem;
        public IntPtr hBalloonIcon;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X, Y; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern ushort RegisterClassExW(ref WNDCLASSEX wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool UnregisterClassW(string name, IntPtr hInstance);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowExW(uint exStyle, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern IntPtr DefWindowProcW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessageW(string name);
    [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] private static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll")] private static extern bool DestroyMenu(IntPtr hMenu);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool AppendMenuW(IntPtr hMenu, uint flags, UIntPtr id, string? item);
    [DllImport("user32.dll")] private static extern bool SetMenuDefaultItem(IntPtr hMenu, uint item, uint byPosition);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern int TrackPopupMenuEx(IntPtr hMenu, uint flags, int x, int y, IntPtr hWnd, IntPtr tpm);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT pt);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadImageW(IntPtr inst, string name, uint type, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(IntPtr hIcon);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern bool Shell_NotifyIconW(uint message, ref NOTIFYICONDATA data);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string? name);

    // ---- public surface ----
    public event Action? OpenRequested, ToggleRequested, CaptureRequested, OpenReportsRequested, ToggleStartupRequested,
                          ExitRequested, ExitAndStopRequested, BalloonClicked;
    public Func<TrayMenuModel>? MenuModelProvider { get; set; }

    private readonly WndProcDelegate _proc;            // kept in a field so the GC cannot collect the callback
    private readonly string _className = "WinFreezeTrace.Manager.Tray." + Guid.NewGuid().ToString("N");
    private readonly IntPtr[] _icons = new IntPtr[4];
    private IntPtr _hwnd, _hInstance;
    private uint _msgTaskbarCreated;
    private bool _added, _v4, _disposed;
    private TrayState _state = TrayState.Stopped;
    private string _tip = "WinFreezeTrace Manager";

    public TrayIcon()
    {
        _proc = WndProc;
        _hInstance = GetModuleHandleW(null);
        _msgTaskbarCreated = RegisterWindowMessageW("TaskbarCreated");
        var wc = new WNDCLASSEX
        {
            cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_proc),
            hInstance = _hInstance,
            lpszClassName = _className,
        };
        RegisterClassExW(ref wc);
        _hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, _className, "WinFreezeTrace Manager tray", WS_POPUP, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, _hInstance, IntPtr.Zero);

        int size = Math.Clamp(GetSystemMetrics(SM_CXSMICON), 16, 64);
        string[] files = { "tray_on.ico", "tray_off.ico", "tray_busy.ico", "tray_stopped.ico" };
        for (int i = 0; i < files.Length; i++)
            _icons[i] = LoadImageW(IntPtr.Zero, AppPaths.AssetPath(files[i]), IMAGE_ICON, size, size, LR_LOADFROMFILE);
        Add();
    }

    public bool IsVisible => _added;

    public void SetState(TrayState state, string tip)
    {
        _state = state;
        _tip = tip.Length > 127 ? tip.Substring(0, 127) : tip;
        if (!_added) return;
        var n = Fill(NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        n.hIcon = _icons[(int)_state];
        n.szTip = _tip;
        Shell_NotifyIconW(NIM_MODIFY, ref n);
    }

    public void ShowBalloon(string title, string text, bool warning)
    {
        if (!_added) return;
        var n = Fill(NIF_INFO);
        n.szInfoTitle = title.Length > 63 ? title.Substring(0, 63) : title;
        n.szInfo = text.Length > 255 ? text.Substring(0, 255) : text;
        n.dwInfoFlags = warning ? NIIF_WARNING : (NIIF_INFO | NIIF_NOSOUND);
        Shell_NotifyIconW(NIM_MODIFY, ref n);
    }

    private NOTIFYICONDATA Fill(uint flags) => new()
    {
        cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATA>(),
        hWnd = _hwnd,
        uID = 1,
        uFlags = flags,
        szTip = "",
        szInfo = "",
        szInfoTitle = "",
    };

    private bool Add()
    {
        if (_hwnd == IntPtr.Zero || _disposed) return false;
        var n = Fill(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        n.uCallbackMessage = WM_TRAY;
        n.hIcon = _icons[(int)_state];
        n.szTip = _tip;
        if (!Shell_NotifyIconW(NIM_ADD, ref n)) { Log.Warn("tray icon could not be added (is Explorer running?); it is retried when the taskbar appears"); return false; }
        _added = true;
        var v = Fill(0);
        v.uVersion = NOTIFYICON_VERSION_4;
        _v4 = Shell_NotifyIconW(NIM_SETVERSION, ref v);       // modern mouse/keyboard semantics
        return true;
    }

    private void Remove()
    {
        if (!_added) return;
        var n = Fill(0);
        Shell_NotifyIconW(NIM_DELETE, ref n);
        _added = false;
    }

    private IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            if (_msgTaskbarCreated != 0 && msg == _msgTaskbarCreated)      // Explorer restarted: its tray was wiped
            {
                _added = false;
                if (Add()) Log.Info("tray icon restored after the taskbar was re-created");
                return IntPtr.Zero;
            }
            if (msg == WM_TRAY) { OnTray(wParam, lParam); return IntPtr.Zero; }
        }
        catch (Exception ex) { Log.Error("tray callback failed: " + ex); }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    private void OnTray(IntPtr wParam, IntPtr lParam)
    {
        uint ev = _v4 ? (uint)((long)lParam & 0xFFFF) : (uint)(long)lParam;
        if (_v4)
        {
            switch (ev)
            {
                case NIN_SELECT: case NIN_KEYSELECT: OpenRequested?.Invoke(); break;
                case WM_CONTEXTMENU:
                    ShowMenu((short)((long)wParam & 0xFFFF), (short)(((long)wParam >> 16) & 0xFFFF));
                    break;
                case NIN_BALLOONUSERCLICK: BalloonClicked?.Invoke(); break;
            }
        }
        else                                                                // legacy callback semantics
        {
            const uint WM_LBUTTONUP = 0x0202, WM_RBUTTONUP = 0x0205;
            GetCursorPos(out var pt);
            if (ev == WM_LBUTTONUP) OpenRequested?.Invoke();
            else if (ev == WM_RBUTTONUP) ShowMenu(pt.X, pt.Y);
            else if (ev == NIN_BALLOONUSERCLICK) BalloonClicked?.Invoke();
        }
    }

    private void ShowMenu(int x, int y)
    {
        var m = MenuModelProvider?.Invoke() ?? new TrayMenuModel();
        IntPtr menu = CreatePopupMenu();
        if (menu == IntPtr.Zero) return;

        AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, UIntPtr.Zero, m.Header);
        AppendMenuW(menu, MF_SEPARATOR, UIntPtr.Zero, null);
        AppendMenuW(menu, MF_STRING, (UIntPtr)ID_OPEN, "&Open WinFreezeTrace Manager");
        string toggle = !m.RecorderRunning ? "&Start the recorder" : (m.Paused ? "Turn monitoring &ON" : "Turn monitoring &OFF");
        AppendMenuW(menu, MF_STRING, (UIntPtr)ID_TOGGLE, toggle);
        AppendMenuW(menu, MF_STRING | ((m.RecorderRunning && !m.Paused && !m.Capturing) ? 0u : (MF_GRAYED | MF_DISABLED)), (UIntPtr)ID_CAPTURE, "&Capture a report now");
        AppendMenuW(menu, MF_STRING, (UIntPtr)ID_FOLDER, "Open &reports folder");
        AppendMenuW(menu, MF_SEPARATOR, UIntPtr.Zero, null);
        AppendMenuW(menu, MF_STRING | (m.StartupEnabled ? MF_CHECKED : 0u), (UIntPtr)ID_STARTUP, "&Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, UIntPtr.Zero, null);
        AppendMenuW(menu, MF_STRING, (UIntPtr)ID_EXIT, "E&xit Manager");
        if (m.RecorderRunning) AppendMenuW(menu, MF_STRING, (UIntPtr)ID_EXIT_STOP, "Exit and s&top the recorder");
        SetMenuDefaultItem(menu, ID_OPEN, 0);

        SetForegroundWindow(_hwnd);                         // otherwise the menu does not close when the user clicks elsewhere
        uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? TPM_RIGHTALIGN : 0u);
        int cmd = TrackPopupMenuEx(menu, flags, x, y, _hwnd, IntPtr.Zero);
        PostMessageW(_hwnd, WM_NULL, IntPtr.Zero, IntPtr.Zero);
        DestroyMenu(menu);

        switch (cmd)
        {
            case ID_OPEN: OpenRequested?.Invoke(); break;
            case ID_TOGGLE: ToggleRequested?.Invoke(); break;
            case ID_CAPTURE: CaptureRequested?.Invoke(); break;
            case ID_FOLDER: OpenReportsRequested?.Invoke(); break;
            case ID_STARTUP: ToggleStartupRequested?.Invoke(); break;
            case ID_EXIT: ExitRequested?.Invoke(); break;
            case ID_EXIT_STOP: ExitAndStopRequested?.Invoke(); break;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        Remove();
        _disposed = true;
        if (_hwnd != IntPtr.Zero) { DestroyWindow(_hwnd); _hwnd = IntPtr.Zero; }
        UnregisterClassW(_className, _hInstance);
        foreach (var i in _icons.Distinct()) if (i != IntPtr.Zero) DestroyIcon(i);
    }
}
