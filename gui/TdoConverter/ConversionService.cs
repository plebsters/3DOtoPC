using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace TdoConverter;

public enum Stage { Open, Extract, Analyze, Emit, Configure, Build, Done }

public sealed record ConversionRequest(
    string DiscPath, string OutputDir, string? GameName, string? HintsPath,
    string? LaunchMe, bool Build, bool ScanPrologues, bool Release)
{
    /// Discs 2, 3, ... of a multi-disc game (unpacked to data_disc2, ...).
    public IReadOnlyList<string> ExtraDiscs { get; init; } = Array.Empty<string>();
    /// A picture chosen for the cover (window and exe icon); null finds one.
    public string? CoverPath { get; init; }
}

/// <summary>Runs the native pipeline on a background thread. Everything is local.</summary>
public sealed class ConversionService
{
    public event Action<int, string>? Log;            // level, message
    public event Action<Stage, float>? Progress;

    public Task<(int rc, string source)> FindCoverAsync(string disc, string outPng) =>
        Task.Run(() => NativeMethods.FindCover(disc, outPng));

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
        // As the command line does: other discs first (seconds; the build takes
        // minutes), and disc folders left from an earlier conversion removed.
        for (int k = r.ExtraDiscs.Count + 2; k < 16; ++k)
        {
            string stale = Path.Combine(r.OutputDir, "data_disc" + k);
            if (Directory.Exists(stale))
            {
                try { Directory.Delete(stale, true); Log?.Invoke(0, $"Removed {stale} (from an earlier conversion)"); } catch { }
            }
        }
        for (int k = 0; k < r.ExtraDiscs.Count; ++k)
        {
            Log?.Invoke(0, $"Disc {k + 2}: {r.ExtraDiscs[k]}");
            IntPtr src = NativeMethods.Utf8(r.ExtraDiscs[k]), dst = NativeMethods.Utf8(Path.Combine(r.OutputDir, "data_disc" + (k + 2)));
            try { int rc = NativeMethods.tdo_extract_disc(src, dst, log, IntPtr.Zero); if (rc != 0) return rc; }
            finally { NativeMethods.Free(src); NativeMethods.Free(dst); }
        }
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
            CoverPath = NativeMethods.Utf8(string.IsNullOrWhiteSpace(r.CoverPath) ? null : r.CoverPath),
        };
        try { return NativeMethods.tdo_convert(ref o, log, prog, IntPtr.Zero); }
        finally
        {
            NativeMethods.Free(o.DiscPath); NativeMethods.Free(o.OutputDir); NativeMethods.Free(o.GameName);
            NativeMethods.Free(o.HintsPath); NativeMethods.Free(o.LaunchMe); NativeMethods.Free(o.CoverPath);
            GC.KeepAlive(log); GC.KeepAlive(prog);
        }
    });

    private void OnLog(int level, IntPtr msg, IntPtr _) =>
        Log?.Invoke(level, Marshal.PtrToStringUTF8(msg) ?? string.Empty);
}
