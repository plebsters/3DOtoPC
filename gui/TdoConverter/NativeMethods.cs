using System;
using System.Runtime.InteropServices;
using System.Text;

namespace TdoConverter;

/// <summary>P/Invoke bindings for core/include/tdo/api.h (tdocore.dll).</summary>
internal static class NativeMethods
{
    private const string Dll = "tdocore";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LogCallback(int level, IntPtr message, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ProgressCallback(int stage, float fraction, IntPtr user);

    [StructLayout(LayoutKind.Sequential)]
    public struct ConvertOptions
    {
        public IntPtr DiscPath, OutputDir, GameName, RuntimeDir, HintsPath, LaunchMe;
        public int Build, ScanPrologues, Release;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr tdo_version();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int tdo_inspect(IntPtr discPath, LogCallback log, IntPtr user);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int tdo_convert(ref ConvertOptions opts, LogCallback log, ProgressCallback progress, IntPtr user);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int tdo_detect_toolchain(byte[] buf, int len);

    // Another disc of a multi-disc game: its files go to destDir (data_disc2, ...).
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int tdo_extract_disc(IntPtr discPath, IntPtr destDir, LogCallback log, IntPtr user);

    public static string Version() => Marshal.PtrToStringUTF8(tdo_version()) ?? "?";

    public static (bool found, string description) DetectToolchain()
    {
        var buf = new byte[2048];
        int rc = tdo_detect_toolchain(buf, buf.Length);
        int len = Array.IndexOf(buf, (byte)0);
        return (rc == 0, Encoding.UTF8.GetString(buf, 0, len < 0 ? buf.Length : len));
    }

    public static IntPtr Utf8(string? s) => s is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(s);
    public static void Free(IntPtr p) { if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p); }
}
