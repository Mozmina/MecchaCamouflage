using System.Runtime.InteropServices;
using System.Runtime.Intrinsics.X86;

namespace ZemiMecchamouflage.Controller;

public sealed record RuntimePlatformCapabilities(
    string OsDescription,
    Version OsVersion,
    Architecture ProcessArchitecture,
    Architecture OsArchitecture,
    bool Sse2Supported,
    bool AvxSupported,
    bool Avx2Supported);

public static class RuntimePlatformDiagnostics
{
    public static RuntimePlatformCapabilities Capture() =>
        new(
            RuntimeInformation.OSDescription,
            Environment.OSVersion.Version,
            RuntimeInformation.ProcessArchitecture,
            RuntimeInformation.OSArchitecture,
            Sse2.IsSupported,
            Avx.IsSupported,
            Avx2.IsSupported);

    public static string FormatCurrent() => Format(Capture());

    public static string Format(RuntimePlatformCapabilities capabilities)
    {
        var adaptiveAvx2 =
            capabilities.Sse2Supported &&
            capabilities.AvxSupported &&
            capabilities.Avx2Supported;
        return
            $"System: os={Normalize(capabilities.OsDescription)} " +
            $"{capabilities.OsVersion} | " +
            $"arch=process:{ArchitectureName(capabilities.ProcessArchitecture)} " +
            $"os:{ArchitectureName(capabilities.OsArchitecture)} | " +
            $"cpu_isa=sse2:{State(capabilities.Sse2Supported)} " +
            $"avx:{State(capabilities.AvxSupported)} " +
            $"avx2:{State(capabilities.Avx2Supported)} | " +
            $"native=x64/sse2 adaptive_avx2:{State(adaptiveAvx2)}";
    }

    private static string ArchitectureName(Architecture architecture) =>
        architecture.ToString().ToLowerInvariant();

    private static string State(bool supported) => supported ? "on" : "off";

    private static string Normalize(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return "unknown";
        var fields = value
            .Replace('|', ' ')
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        return fields.Length == 0 ? "unknown" : string.Join(' ', fields);
    }
}
