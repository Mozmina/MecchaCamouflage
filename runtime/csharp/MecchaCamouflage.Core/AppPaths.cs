using System.Security.Cryptography;
using System.Text;

namespace MecchaCamouflage.Core;

public sealed class AppPaths
{
    public AppPaths(string version)
    {
        Version = SanitizeVersion(version);
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(local))
            local = Path.GetTempPath();
        VersionRoot = Path.Combine(local, "MecchaCamouflage", "versions", Version);
        ConfigDirectory = Path.Combine(VersionRoot, "config");
        ConfigPath = Path.Combine(ConfigDirectory, "config.json");
        LegacyConfigPath = Path.Combine(VersionRoot, "config.json");
        LogDirectory = Path.Combine(VersionRoot, "logs");
        RuntimeDirectory = Path.Combine(VersionRoot, "runtime");
        RuntimeBinDirectory = Path.Combine(RuntimeDirectory, "bin");
        ProgressDirectory = Path.Combine(RuntimeDirectory, "progress");
        DebugDirectory = Path.Combine(VersionRoot, "debug");
    }

    public string Version { get; }
    public string VersionRoot { get; }
    public string ConfigDirectory { get; }
    public string ConfigPath { get; }
    public string LegacyConfigPath { get; }
    public string LogDirectory { get; }
    public string RuntimeDirectory { get; }
    public string RuntimeBinDirectory { get; }
    public string ProgressDirectory { get; }
    public string DebugDirectory { get; }

    public void EnsureBaseDirectories()
    {
        Directory.CreateDirectory(ConfigDirectory);
        Directory.CreateDirectory(LogDirectory);
        Directory.CreateDirectory(RuntimeBinDirectory);
        Directory.CreateDirectory(ProgressDirectory);
    }

    public string RuntimeHashDirectory(string bridgePath, string injectorPath)
    {
        using var sha = SHA256.Create();
        foreach (var path in new[] { bridgePath, injectorPath })
        {
            var bytes = File.ReadAllBytes(path);
            sha.TransformBlock(bytes, 0, bytes.Length, null, 0);
        }
        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        var hash = Convert.ToHexString(sha.Hash ?? Array.Empty<byte>()).ToLowerInvariant()[..16];
        return Path.Combine(RuntimeBinDirectory, hash);
    }

    public void CleanupRuntimeBinDirectories(string keepDirectory, TimeSpan maxAge, int keepNewest = 3)
    {
        Directory.CreateDirectory(RuntimeBinDirectory);
        var keepFullPath = NormalizeDirectory(keepDirectory);
        var cutoff = DateTime.UtcNow - maxAge;
        var directories = Directory.GetDirectories(RuntimeBinDirectory)
            .Select(path => new DirectoryInfo(path))
            .Where(info => !string.Equals(NormalizeDirectory(info.FullName), keepFullPath, StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(info => info.LastWriteTimeUtc)
            .ToArray();

        for (var index = 0; index < directories.Length; ++index)
        {
            var expired = directories[index].LastWriteTimeUtc < cutoff;
            var overLimit = index >= Math.Max(0, keepNewest);
            if (!expired && !overLimit)
                continue;
            try
            {
                directories[index].Delete(recursive: true);
            }
            catch
            {
                // A loaded bridge DLL can keep a directory locked. It is safe to retry next startup.
            }
        }
    }

    public static string SanitizeVersion(string? version)
    {
        var input = string.IsNullOrWhiteSpace(version) ? "unversioned" : version.Trim();
        var builder = new StringBuilder(input.Length);
        foreach (var ch in input)
        {
            builder.Append(char.IsLetterOrDigit(ch) || ch is '.' or '_' or '-' ? ch : '_');
        }
        return builder.Length == 0 ? "unversioned" : builder.ToString();
    }

    private static string NormalizeDirectory(string path) =>
        Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
}
