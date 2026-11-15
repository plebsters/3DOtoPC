using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace TdoConverter;

public enum Stage { Open, Extract, Analyze, Emit, Configure, Build, Done }

public sealed record ConversionRequest(
    string DiscPath, string OutputDir, string? GameName, string? HintsPath,
    string? LaunchMe, bool Build, bool ScanPrologues, bool Release);

/// <summary>Runs the native pipeline on a background thread. Everything is local.</summary>
public sealed class ConversionService
{
    public event Action<int, string>? Log;            // level, message
    public event Action<Stage, float>? Progress;

    public Task<int> InspectAsync(string disc) => Task.Run(() =>
    {
        NativeMethods.LogCallback log = OnLog;        // keep delegate alive for the call
        IntPtr p = NativeMethods.Utf8(disc);
        try { return NativeMethods.tdo_inspect(p, log, IntPtr.Zero); }
        finally { NativeMethods.Free(p); GC.KeepAlive(log); }
    });

    public Task<int> ConvertAsync(ConversionRequest r) => Task.Run(() =>
    {
        NativeMethods.LogCallback log = OnLog;
        NativeMethods.ProgressCallback prog = (s, f, _) => Progress?.Invoke((Stage)s, f);
        var o = new NativeMethods.ConvertOptions
        {
            DiscPath = NativeMethods.Utf8(r.DiscPath),
            OutputDir = NativeMethods.Utf8(r.OutputDir),
            GameName = NativeMethods.Utf8(string.IsNullOrWhiteSpace(r.GameName) ? null : r.GameName),
            RuntimeDir = IntPtr.Zero,                 // default: <app dir>\runtime
            HintsPath = NativeMethods.Utf8(string.IsNullOrWhiteSpace(r.HintsPath) ? null : r.HintsPath),
            LaunchMe = NativeMethods.Utf8(string.IsNullOrWhiteSpace(r.LaunchMe) ? null : r.LaunchMe),
            Build = r.Build ? 1 : 0,
            ScanPrologues = r.ScanPrologues ? 1 : 0,
            Release = r.Release ? 1 : 0,
        };
        try { return NativeMethods.tdo_convert(ref o, log, prog, IntPtr.Zero); }
        finally
        {
            NativeMethods.Free(o.DiscPath); NativeMethods.Free(o.OutputDir); NativeMethods.Free(o.GameName);
            NativeMethods.Free(o.HintsPath); NativeMethods.Free(o.LaunchMe);
            GC.KeepAlive(log); GC.KeepAlive(prog);
        }
    });

    private void OnLog(int level, IntPtr msg, IntPtr _) =>
        Log?.Invoke(level, Marshal.PtrToStringUTF8(msg) ?? string.Empty);
}
