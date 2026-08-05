using System.Security.Cryptography;
using System.Text;
using ZemiMecchamouflage.Core;

namespace ZemiMecchamouflage.Controller;

public sealed record NativeRuntimeBundleFile(string RelativePath, string Sha256);

public sealed record NativeRuntimeBundleDescriptor(
    string Id,
    string BridgeSha256,
    string CanonicalManifest,
    IReadOnlyList<NativeRuntimeBundleFile> Profiles);

/// <summary>
/// Content identity for every file and compatibility boundary that changes the
/// behavior of a process-resident native bridge generation.
/// </summary>
public static class NativeRuntimeBundle
{
    public const uint SchemaVersion = 1;
    public const uint StartBlockAbi = 2;
    public const uint ResidentCoreAbi = 2;
    public const uint ProtocolVersion = 2;

    public static NativeRuntimeBundleDescriptor CreatePackaged(AppPaths paths, RuntimeLog? log = null)
    {
        ArgumentNullException.ThrowIfNull(paths);
        var nativeRoot = PackagedAssets.ResolveRequiredAssetRoot(paths, "native", log);
        var profilesRoot = PackagedAssets.ResolveRequiredAssetRoot(paths, "web", log);
        var nestedBridge = Path.Combine(nativeRoot, "native", "runtime-bridge.dll");
        var bridgePath = File.Exists(nestedBridge)
            ? nestedBridge
            : Path.Combine(nativeRoot, "runtime-bridge.dll");
        return Create(
            bridgePath,
            Path.Combine(profilesRoot, "web", "mesh-profiles"));
    }

    public static NativeRuntimeBundleDescriptor Create(string bridgePath, string profileDirectory)
    {
        if (string.IsNullOrWhiteSpace(bridgePath))
            throw new ArgumentException("A native bridge path is required.", nameof(bridgePath));
        if (string.IsNullOrWhiteSpace(profileDirectory))
            throw new ArgumentException("A runtime profile directory is required.", nameof(profileDirectory));

        var normalizedBridgePath = Path.GetFullPath(bridgePath);
        var normalizedProfileDirectory = Path.GetFullPath(profileDirectory);
        if (!File.Exists(normalizedBridgePath))
            throw new FileNotFoundException("The native bridge DLL is missing.", normalizedBridgePath);
        if (!Directory.Exists(normalizedProfileDirectory))
            throw new DirectoryNotFoundException("The runtime profile directory is missing: " + normalizedProfileDirectory);

        var profiles = Directory
            .EnumerateFiles(normalizedProfileDirectory, "*.json", SearchOption.TopDirectoryOnly)
            .Select(path => new NativeRuntimeBundleFile(
                "mesh-profiles/" + Path.GetFileName(path),
                PackagedAssets.Sha256File(path).ToLowerInvariant()))
            .OrderBy(entry => entry.RelativePath, StringComparer.Ordinal)
            .ToArray();
        if (profiles.Length == 0)
            throw new InvalidDataException("The runtime bundle contains no mesh or image profiles.");
        for (var index = 1; index < profiles.Length; ++index)
        {
            if (string.Equals(profiles[index - 1].RelativePath, profiles[index].RelativePath, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("The runtime bundle contains profile names that differ only by case.");
        }

        var bridgeSha256 = PackagedAssets.Sha256File(normalizedBridgePath).ToLowerInvariant();
        var manifest = new StringBuilder()
            .Append("schema=").Append(SchemaVersion).Append('\n')
            .Append("start_block_abi=").Append(StartBlockAbi).Append('\n')
            .Append("resident_core_abi=").Append(ResidentCoreAbi).Append('\n')
            .Append("protocol=").Append(ProtocolVersion).Append('\n')
            .Append("bridge=").Append(bridgeSha256).Append('\n');
        foreach (var profile in profiles)
            manifest.Append("profile=").Append(profile.RelativePath).Append('=').Append(profile.Sha256).Append('\n');

        var canonicalManifest = manifest.ToString();
        var id = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(canonicalManifest))).ToLowerInvariant();
        return new NativeRuntimeBundleDescriptor(id, bridgeSha256, canonicalManifest, profiles);
    }
}
