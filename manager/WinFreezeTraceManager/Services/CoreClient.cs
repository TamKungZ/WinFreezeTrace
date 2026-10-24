using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace WinFreezeTraceManager.Services;

/// <summary>What the recorder reported. Running == false means nothing answered on the control pipe.</summary>
internal sealed class CoreStatus
{
    public bool Running { get; init; }
    public bool Ok { get; init; }
    public string Error { get; init; } = "";
    public string Version { get; init; } = "";
    public int Pid { get; init; }
    public bool Paused { get; init; }
    public bool Capturing { get; init; }
    public bool Hotkey { get; init; }
    public int Captures { get; init; }
    public int Failures { get; init; }
    public bool LastOk { get; init; }
    public string LastReport { get; init; } = "";
    public string ReportsDir { get; init; } = "";

    public static CoreStatus NotRunning { get; } = new();
}

/// <summary>Client for the recorder's control pipe (see src/win/control.h): one request line, one JSON reply line.</summary>
internal static class CoreClient
{
    public static string PipeName => "WinFreezeTrace.Control.v1." + Process.GetCurrentProcess().SessionId;

    public static async Task<CoreStatus> SendAsync(string command, int connectTimeoutMs = 400)
    {
        string line;
        try
        {
            using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            using var cts = new CancellationTokenSource(connectTimeoutMs + 3000);
            await pipe.ConnectAsync(connectTimeoutMs, cts.Token).ConfigureAwait(true);

            var request = Encoding.ASCII.GetBytes(command + "\n");
            await pipe.WriteAsync(request, cts.Token).ConfigureAwait(true);
            await pipe.FlushAsync(cts.Token).ConfigureAwait(true);

            var buf = new byte[32768];
            int len = 0;
            while (len < buf.Length)
            {
                int n = await pipe.ReadAsync(buf.AsMemory(len), cts.Token).ConfigureAwait(true);
                if (n <= 0) break;
                len += n;
                if (Array.IndexOf(buf, (byte)'\n', 0, len) >= 0) break;
            }
            line = Encoding.UTF8.GetString(buf, 0, len).Trim();
        }
        catch (Exception ex) when (ex is TimeoutException or IOException or UnauthorizedAccessException or OperationCanceledException)
        {
            return CoreStatus.NotRunning;
        }

        try
        {
            using var doc = JsonDocument.Parse(line);
            var r = doc.RootElement;
            return new CoreStatus
            {
                Running = true,
                Ok = Bool(r, "ok"),
                Error = Str(r, "error"),
                Version = Str(r, "version"),
                Pid = Int(r, "pid"),
                Paused = Bool(r, "paused"),
                Capturing = Bool(r, "capturing"),
                Hotkey = Bool(r, "hotkey"),
                Captures = Int(r, "captures"),
                Failures = Int(r, "failures"),
                LastOk = Bool(r, "last_ok"),
                LastReport = Str(r, "last_report"),
                ReportsDir = Str(r, "reports_dir"),
            };
        }
        catch (JsonException ex)
        {
            Log.Warn("unreadable reply from the recorder: " + ex.Message);
            return CoreStatus.NotRunning;
        }
    }

    private static bool Bool(JsonElement r, string name) =>
        r.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
    private static int Int(JsonElement r, string name) =>
        r.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var i) ? i : 0;
    private static string Str(JsonElement r, string name) =>
        r.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
}
