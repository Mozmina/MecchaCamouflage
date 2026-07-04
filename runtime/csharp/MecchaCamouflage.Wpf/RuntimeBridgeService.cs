using System.Diagnostics;
using System.Security.Cryptography;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Wpf;

public sealed class RuntimeBridgeService
{
    public const int BridgePort = 50262;

    private readonly AppPaths paths;
    private readonly RuntimeLog log;
    private readonly BridgeClient client = new(port: BridgePort);

    private string bridgePath = "";
    private string injectorPath = "";
    private string progressPath = "";

    public RuntimeBridgeService(AppPaths paths, RuntimeLog log)
    {
        this.paths = paths;
        this.log = log;
    }

    public string BridgePath => bridgePath;
    public string ProgressPath => progressPath;

    public Process? FindGameProcess(string processName)
    {
        var name = Path.GetFileNameWithoutExtension(processName);
        return Process.GetProcessesByName(name).FirstOrDefault();
    }

    public Task<BridgeReply> PingAsync(CancellationToken cancellationToken = default) =>
        client.PingAsync(cancellationToken);

    public Task<BridgeReply> CancelPaintAsync(CancellationToken cancellationToken = default) =>
        client.CancelPaintAsync(cancellationToken);

    public Task<BridgeReply> SendPaintAsync(string payload, CancellationToken cancellationToken = default) =>
        client.RequestAsync(payload, cancellationToken);

    public async Task<bool> EnsureReadyAsync(string processName, CancellationToken cancellationToken = default)
    {
        var ping = await client.PingAsync(cancellationToken);
        if (ping.Ok && ping.Success)
            return true;

        var process = FindGameProcess(processName);
        if (process is null)
        {
            log.Warn($"Waiting for process {processName}.");
            return false;
        }

        PrepareNativeRuntime();
        File.WriteAllText(bridgePath + ".port", BridgePort + Environment.NewLine);
        log.Info($"Injecting bridge into {process.ProcessName}.exe.");
        var start = new ProcessStartInfo(injectorPath, Quote(processName) + " " + Quote(bridgePath))
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true
        };
        using var injector = Process.Start(start);
        if (injector is null)
            return false;
        await injector.WaitForExitAsync(cancellationToken);
        var stdout = await injector.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderr = await injector.StandardError.ReadToEndAsync(cancellationToken);
        if (injector.ExitCode != 0)
        {
            log.Error($"Bridge injection failed ({injector.ExitCode}): {stderr.Trim()}");
            return false;
        }
        if (!string.IsNullOrWhiteSpace(stdout))
            log.Info(stdout.Trim());

        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var ready = await client.PingAsync(cancellationToken);
            if (ready.Ok && ready.Success)
                return true;
            await Task.Delay(250, cancellationToken);
        }
        log.Warn("Bridge did not become ready after injection.");
        return false;
    }

    private void PrepareNativeRuntime()
    {
        paths.EnsureBaseDirectories();
        var appDir = AppContext.BaseDirectory;
        var packagedNativeDir = Path.Combine(appDir, "native");
        var packagedBridge = Path.Combine(packagedNativeDir, "runtime-bridge.dll");
        var packagedInjector = Path.Combine(packagedNativeDir, "runtime-injector.exe");
        if (!File.Exists(packagedBridge))
            packagedBridge = Path.Combine(appDir, "runtime-bridge.dll");
        if (!File.Exists(packagedInjector))
            packagedInjector = Path.Combine(appDir, "runtime-injector.exe");
        if (!File.Exists(packagedBridge) || !File.Exists(packagedInjector))
            throw new FileNotFoundException("Packaged native bridge or injector is missing.");

        var runtimeDir = paths.RuntimeHashDirectory(packagedBridge, packagedInjector);
        Directory.CreateDirectory(runtimeDir);
        injectorPath = Path.Combine(runtimeDir, "runtime-injector.exe");
        File.Copy(packagedInjector, injectorPath, true);

        var hash = ShortHash(packagedBridge);
        bridgePath = Path.Combine(runtimeDir, $"runtime-bridge-{hash}-{BridgePort}.dll");
        File.Copy(packagedBridge, bridgePath, true);
        Directory.CreateDirectory(paths.ProgressDirectory);
        progressPath = Path.Combine(paths.ProgressDirectory, $"bridge-{hash}-{BridgePort}.progress.json");
        File.WriteAllText(bridgePath + ".progress.path", progressPath + Environment.NewLine);

        var sourceProfiles = Path.Combine(appDir, "mesh-profiles");
        var targetProfiles = Path.Combine(runtimeDir, "mesh-profiles");
        if (Directory.Exists(sourceProfiles))
        {
            Directory.CreateDirectory(targetProfiles);
            foreach (var file in Directory.EnumerateFiles(sourceProfiles, "*.json"))
                File.Copy(file, Path.Combine(targetProfiles, Path.GetFileName(file)), true);
        }
        paths.CleanupRuntimeBinDirectories(runtimeDir, TimeSpan.FromDays(14), keepNewest: 3);
    }

    private static string ShortHash(string path)
    {
        using var sha = SHA256.Create();
        var hash = sha.ComputeHash(File.ReadAllBytes(path));
        return Convert.ToHexString(hash).ToLowerInvariant()[..16];
    }

    private static string Quote(string value) => "\"" + value.Replace("\"", "\\\"") + "\"";
}
