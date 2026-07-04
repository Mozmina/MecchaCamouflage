using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Controller;

public static class PackagedAssets
{
    private const string ResourcePrefix = "packaged/";
    private static readonly object ExtractLock = new();

    public static string ResolveAssetRoot(AppPaths paths, string directoryName)
    {
        var extracted = EnsureExtracted(paths);
        if (!string.IsNullOrEmpty(extracted) && Directory.Exists(Path.Combine(extracted, directoryName)))
            return extracted;
        return AppContext.BaseDirectory;
    }

    private static string? EnsureExtracted(AppPaths paths)
    {
        var assembly = Assembly.GetEntryAssembly() ?? typeof(PackagedAssets).Assembly;
        var resources = assembly.GetManifestResourceNames()
            .Where(name => name.StartsWith(ResourcePrefix, StringComparison.Ordinal))
            .Order(StringComparer.Ordinal)
            .ToArray();
        if (resources.Length == 0)
            return null;

        var hash = ComputeResourceHash(assembly, resources);
        var root = Path.Combine(paths.RuntimeDirectory, "package-assets", hash);
        var marker = Path.Combine(root, ".complete");
        if (File.Exists(marker))
            return root;

        lock (ExtractLock)
        {
            if (File.Exists(marker))
                return root;

            var parent = Path.GetDirectoryName(root)!;
            Directory.CreateDirectory(parent);
            var temp = Path.Combine(parent, hash + "." + Guid.NewGuid().ToString("N") + ".tmp");
            Directory.CreateDirectory(temp);
            try
            {
                ExtractResources(assembly, resources, temp);
                File.WriteAllText(Path.Combine(temp, ".complete"), DateTimeOffset.UtcNow.ToString("O") + Environment.NewLine);
                if (Directory.Exists(root))
                    Directory.Delete(root, recursive: true);
                Directory.Move(temp, root);
                CleanupOldAssetDirectories(parent, root);
            }
            catch
            {
                TryDeleteDirectory(temp);
                throw;
            }
        }
        return root;
    }

    private static string ComputeResourceHash(Assembly assembly, string[] resources)
    {
        using var sha = SHA256.Create();
        var buffer = new byte[81920];
        foreach (var resource in resources)
        {
            var nameBytes = Encoding.UTF8.GetBytes(resource);
            sha.TransformBlock(nameBytes, 0, nameBytes.Length, null, 0);
            using var stream = assembly.GetManifestResourceStream(resource)
                ?? throw new FileNotFoundException("Packaged resource is missing: " + resource);
            int read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                sha.TransformBlock(buffer, 0, read, null, 0);
        }
        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        return Convert.ToHexString(sha.Hash ?? Array.Empty<byte>()).ToLowerInvariant()[..16];
    }

    private static void ExtractResources(Assembly assembly, string[] resources, string root)
    {
        var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        foreach (var resource in resources)
        {
            var relative = resource[ResourcePrefix.Length..].Replace('/', Path.DirectorySeparatorChar);
            if (Path.IsPathRooted(relative) || relative.Split(Path.DirectorySeparatorChar).Any(part => part == ".."))
                throw new InvalidOperationException("Invalid packaged resource path: " + resource);

            var destination = Path.GetFullPath(Path.Combine(root, relative));
            if (!destination.StartsWith(fullRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Invalid packaged resource path: " + resource);

            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            using var input = assembly.GetManifestResourceStream(resource)
                ?? throw new FileNotFoundException("Packaged resource is missing: " + resource);
            using var output = File.Create(destination);
            input.CopyTo(output);
        }
    }

    private static void CleanupOldAssetDirectories(string parent, string keepDirectory)
    {
        var keep = Path.GetFullPath(keepDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        foreach (var directory in Directory.GetDirectories(parent)
                     .Select(path => new DirectoryInfo(path))
                     .Where(info => !string.Equals(
                         Path.GetFullPath(info.FullName).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                         keep,
                         StringComparison.OrdinalIgnoreCase))
                     .OrderByDescending(info => info.LastWriteTimeUtc)
                     .Skip(2))
        {
            TryDeleteDirectory(directory.FullName);
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
                Directory.Delete(path, recursive: true);
        }
        catch
        {
            // Extracted package assets can be cleaned on a later launch.
        }
    }
}
