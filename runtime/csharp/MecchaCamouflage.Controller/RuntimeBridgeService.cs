using System.Diagnostics;
using System.Security.Cryptography;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Controller;

public sealed class RuntimeBridgeService
{
    public const int BridgePort = 50262;
    public static readonly TimeSpan BridgeProbeTimeout = TimeSpan.FromMilliseconds(300);
    private static readonly int[] BridgePortCandidates = [BridgePort, 50263, 50264, 50265];

    private readonly AppPaths paths;
    private readonly RuntimeLog log;

    private string bridgePath = "";
    private string injectorPath = "";
    private string progressPath = "";
    private string waitingForProcessName = "";
    private string lastBridgeReadyLogKey = "";
    private string lastBridgeMismatchLogKey = "";
    private string lastRuntimeFilesLogKey = "";
    private int lastInjectionProcessId;
    private int activeBridgePort = BridgePort;
    private bool bridgeReadyTimeoutLogged;
    private bool bridgeConnected;

    public RuntimeBridgeService(AppPaths paths, RuntimeLog log)
    {
        this.paths = paths;
        this.log = log;
    }

    public string BridgePath => bridgePath;
    public string ProgressPath => progressPath;
    public bool IsConnected => bridgeConnected;

    public Process? FindGameProcess(string processName)
    {
        var name = Path.GetFileNameWithoutExtension(processName);
        return Process.GetProcessesByName(name).FirstOrDefault();
    }

    public Task<BridgeReply> PingAsync(CancellationToken cancellationToken = default, TimeSpan? timeoutOverride = null) =>
        Client(activeBridgePort).PingAsync(cancellationToken, timeoutOverride);

    public Task<BridgeReply> CancelPaintAsync(CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).CancelPaintAsync(cancellationToken);

    public Task<BridgeReply> SendPaintAsync(string payload, CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).RequestAsync(payload, cancellationToken);

    public Task<BridgeReply> ShutdownAsync(CancellationToken cancellationToken = default) =>
        Client(activeBridgePort).ShutdownAsync(cancellationToken);

    public async Task<bool> EnsureReadyAsync(string processName, CancellationToken cancellationToken = default)
    {
        var process = FindGameProcess(processName);
        if (process is null)
        {
            bridgeConnected = false;
            if (!string.Equals(waitingForProcessName, processName, StringComparison.OrdinalIgnoreCase))
            {
                log.Warn($"Game process: waiting for {processName}.");
                waitingForProcessName = processName;
            }
            return false;
        }
        waitingForProcessName = "";

        var occupiedPorts = new HashSet<int>();
        foreach (var port in OrderedPortCandidates())
        {
            var ping = await Client(port).PingAsync(cancellationToken, BridgeProbeTimeout);
            if (IsBridgeReadyForProcess(ping, process.Id))
            {
                activeBridgePort = port;
                if (!ValidateReadyBridge(process, port))
                    return false;
                bridgeReadyTimeoutLogged = false;
                waitingForProcessName = "";
                RestoreLoadedBridgeState(process.Id, port);
                DiagnosticsState.SetBridgeInjection($"ready pid={process.Id} port={port}");
                LogBridgeConnected(process.Id, port);
                return true;
            }
            if (ping.Ok && ping.Success && ping.ProcessId is not null && ping.ProcessId != process.Id)
                occupiedPorts.Add(port);
        }

        var targetPort = BridgePortCandidates.FirstOrDefault(port => !occupiedPorts.Contains(port));
        if (targetPort == 0)
        {
            DiagnosticsState.SetLastCode("MC-IPC-201", "all bridge ports are owned by other target processes");
            log.Error("Bridge: communication ports are already in use by other game processes.");
            return false;
        }
        if (await TryInjectAndWaitAsync(processName, process, targetPort, cancellationToken))
        {
            activeBridgePort = targetPort;
            bridgeReadyTimeoutLogged = false;
            return true;
        }
        if (!bridgeReadyTimeoutLogged)
        {
            log.Warn("Bridge: not connected after injection.");
            bridgeReadyTimeoutLogged = true;
        }
        return false;
    }

    private async Task<bool> TryInjectAndWaitAsync(string processName, Process process, int port, CancellationToken cancellationToken)
    {
        var mutexName = $@"Local\MecchaCamouflage.Inject.{process.Id}";
        using var mutex = new Mutex(false, mutexName);
        if (!mutex.WaitOne(TimeSpan.FromSeconds(1)))
        {
            DiagnosticsState.SetBridgeInjection($"waiting for another injection pid={process.Id}");
            return false;
        }
        try
        {
            if (!PrepareNativeRuntimeForPort(port))
                return false;

            var loadedBridges = FindLoadedBridgePaths(process);
            if (loadedBridges.Any(path => SamePath(path, bridgePath)))
            {
                DiagnosticsState.SetBridgeInjection($"bridge already loaded pid={process.Id}");
                log.Info("Bridge: already loaded; waiting for connection.");
                return await WaitForReadyOrLoadedButNotReadyAsync(process, port, cancellationToken);
            }
            if (loadedBridges.Count > 0)
                log.Warn("Bridge: previous DLL is loaded but not responding; injecting current bridge.");

            if (lastInjectionProcessId != process.Id || activeBridgePort != port)
            {
                log.Info($"Bridge: injecting into game process pid={process.Id}.");
                lastInjectionProcessId = process.Id;
            }
            LogTargetProcess(process);
            DiagnosticsState.SetBridgeInjection($"injecting pid={process.Id} port={port}");

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
            LogInjectorOutput(stdout, stderr);
            if (injector.ExitCode != 0)
            {
                DiagnosticsState.SetLastCode(ClassifyInjectionFailure(injector.ExitCode, stderr + stdout), stderr.Trim());
                DiagnosticsState.SetBridgeInjection($"failed exit={injector.ExitCode}");
                log.Error($"Bridge: injection failed ({injector.ExitCode}): {FriendlyInjectorFailure(injector.ExitCode, stderr)}");
                return false;
            }
            DiagnosticsState.SetBridgeInjection($"loaded pid={process.Id}");
            log.Info("Bridge: injected.");
            return await WaitForReadyOrLoadedButNotReadyAsync(process, port, cancellationToken);
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or System.ComponentModel.Win32Exception)
        {
            DiagnosticsState.SetLastCode("MC-INJ-130", ex.Message);
            DiagnosticsState.SetBridgeInjection("injector launch failed");
            log.Error("Bridge: injector could not run: " + FriendlyAccessFailure(ex.Message));
            return false;
        }
        finally
        {
            try { mutex.ReleaseMutex(); } catch (ApplicationException) { }
        }
    }

    private async Task<bool> WaitForReadyOrLoadedButNotReadyAsync(Process process, int port, CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var ready = await Client(port).PingAsync(cancellationToken);
            if (IsBridgeReadyForProcess(ready, process.Id))
            {
                RestoreLoadedBridgeState(process.Id, port);
                DiagnosticsState.SetBridgeInjection($"ready pid={process.Id} port={port}");
                LogBridgeConnected(process.Id, port);
                return true;
            }
            await Task.Delay(250, cancellationToken);
        }
        var listener = ReadListenerStatus();
        DiagnosticsState.SetLastCode("MC-INJ-145", listener.Length == 0 ? "bridge loaded but listener did not become ready" : listener);
        DiagnosticsState.SetBridgeInjection("loaded-but-not-ready");
        log.Error("Bridge: loaded but not connected. " + (listener.Length == 0 ? "Restart the game before retrying." : "Listener status: " + listener));
        return false;
    }

    private bool ValidateReadyBridge(Process process, int port)
    {
        if (!PrepareNativeRuntimeForPort(port))
            return false;

        var loadedBridges = FindLoadedBridgePaths(process);
        if (loadedBridges.Any(path => SamePath(path, bridgePath)))
            return true;

        var detail = loadedBridges.Count == 0
            ? "loaded bridge module could not be confirmed"
            : "loaded=" + string.Join(";", loadedBridges.Select(Path.GetFileName));
        bridgeConnected = false;
        DiagnosticsState.SetLastCode("MC-INJ-146", detail);
        DiagnosticsState.SetBridgeInjection($"different bridge loaded pid={process.Id} port={port}");
        var key = $"{process.Id}:{port}:{detail}";
        if (!string.Equals(lastBridgeMismatchLogKey, key, StringComparison.Ordinal))
        {
            lastBridgeMismatchLogKey = key;
            log.Error("Bridge: a different bridge DLL is already loaded in the game. Restart the game before retrying.");
        }
        return false;
    }

    private bool PrepareNativeRuntimeForPort(int port)
    {
        try
        {
            PrepareNativeRuntime(port);
            File.WriteAllText(bridgePath + ".port", port + Environment.NewLine);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException)
        {
            bridgeConnected = false;
            DiagnosticsState.SetLastCode("MC-RT-001", ex.Message);
            log.Error("Bridge: runtime files could not be prepared: " + FriendlyAccessFailure(ex.Message));
            return false;
        }
    }

    private void PrepareNativeRuntime(int port)
    {
        paths.EnsureBaseDirectories();
        var appDir = PackagedAssets.ResolveRequiredAssetRoot(paths, "native", log);
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
        PackagedAssets.CopyIfInvalid(packagedInjector, injectorPath);

        var hash = ShortHash(packagedBridge);
        bridgePath = Path.Combine(runtimeDir, $"runtime-bridge-{hash}-{port}.dll");
        PackagedAssets.CopyIfInvalid(packagedBridge, bridgePath);
        LogRuntimeFilesPrepared(runtimeDir);
        Directory.CreateDirectory(paths.BridgeProgressDirectory);
        progressPath = Path.Combine(paths.BridgeProgressDirectory, $"bridge-{hash}-{port}.progress.json");
        File.WriteAllText(bridgePath + ".progress.path", progressPath + Environment.NewLine);

        var profileRoot = PackagedAssets.ResolveRequiredAssetRoot(paths, "mesh-profiles", log);
        var sourceProfiles = Path.Combine(profileRoot, "mesh-profiles");
        var targetProfiles = Path.Combine(runtimeDir, "mesh-profiles");
        if (Directory.Exists(sourceProfiles))
        {
            Directory.CreateDirectory(targetProfiles);
            foreach (var file in Directory.EnumerateFiles(sourceProfiles, "*.json"))
                File.Copy(file, Path.Combine(targetProfiles, Path.GetFileName(file)), true);
        }
        paths.CleanupRuntimeBinDirectories(runtimeDir, TimeSpan.FromDays(14), keepNewest: 3);
    }

    private IEnumerable<int> OrderedPortCandidates()
    {
        yield return activeBridgePort;
        foreach (var port in BridgePortCandidates)
        {
            if (port != activeBridgePort)
                yield return port;
        }
    }

    private static bool IsBridgeReadyForProcess(BridgeReply reply, int processId) =>
        reply.Ok &&
        reply.Success &&
        (reply.ProcessId is null || reply.ProcessId == processId);

    private static BridgeClient Client(int port) => new(port: port);

    private static string FriendlyInjectorFailure(int exitCode, string stderr)
    {
        var message = stderr.Trim();
        var lower = message.ToLowerInvariant();
        if (exitCode is 2 or 5 ||
            lower.Contains("access is denied") ||
            lower.Contains("access denied") ||
            lower.Contains("win32=5"))
        {
            return "access denied while opening or injecting into the game process. Run Meccha Camouflage with the same privileges as the game, or try Run as administrator.";
        }
        if (string.IsNullOrWhiteSpace(message))
            return "injector exited without an error message.";
        return message;
    }

    private static string FriendlyAccessFailure(string message)
    {
        var lower = message.ToLowerInvariant();
        if (lower.Contains("access") || lower.Contains("permission") || lower.Contains("denied"))
            return message + " Run Meccha Camouflage with access to its runtime folder.";
        return message;
    }

    private static string ShortHash(string path)
    {
        using var sha = SHA256.Create();
        var hash = sha.ComputeHash(File.ReadAllBytes(path));
        return Convert.ToHexString(hash).ToLowerInvariant()[..16];
    }

    private void LogRuntimeFilesPrepared(string runtimeDir)
    {
        if (string.Equals(lastRuntimeFilesLogKey, runtimeDir, StringComparison.OrdinalIgnoreCase))
            return;
        lastRuntimeFilesLogKey = runtimeDir;
        log.Info("Bridge: runtime files prepared.");
        if (!ResearchArtifactsEnabled())
            return;
        LogRuntimeFileDetails("bridge", bridgePath);
        LogRuntimeFileDetails("injector", injectorPath);
    }

    private void LogRuntimeFileDetails(string label, string path)
    {
        var info = new FileInfo(path);
        log.Info($"Bridge detail: {label} {path} ({info.Length} bytes sha256={PackagedAssets.Sha256File(path)[..16]})");
    }

    private void LogTargetProcess(Process process)
    {
        try
        {
            log.Info($"Game process: found pid={process.Id}.");
            if (ResearchArtifactsEnabled())
                log.Info($"Game process detail: started={process.StartTime:O} path={process.MainModule?.FileName ?? process.ProcessName}");
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            log.Info($"Game process: found pid={process.Id}.");
            if (ResearchArtifactsEnabled())
                log.Warn($"Game process detail unavailable: pid={process.Id} ({ex.Message})");
        }
    }

    private void LogBridgeConnected(int processId, int port)
    {
        bridgeConnected = true;
        var key = $"{processId}:{port}";
        if (string.Equals(lastBridgeReadyLogKey, key, StringComparison.Ordinal))
            return;
        lastBridgeReadyLogKey = key;
        log.Info($"Bridge: connected to game process pid={processId} port={port}.");
    }

    private void LogInjectorOutput(string stdout, string stderr)
    {
        if (ResearchArtifactsEnabled())
        {
            foreach (var line in SplitLines(stdout))
                log.Info("Bridge detail: injector " + line);
        }
        foreach (var line in SplitLines(stderr))
            log.Warn("Bridge detail: injector " + line);
    }

    private static bool ResearchArtifactsEnabled() =>
        string.Equals(Environment.GetEnvironmentVariable("MECCHA_RESEARCH_ARTIFACTS"), "1", StringComparison.Ordinal);

    private void RestoreLoadedBridgeState(int processId, int port)
    {
        if (!string.IsNullOrWhiteSpace(bridgePath) && !string.IsNullOrWhiteSpace(progressPath))
            return;
        try
        {
            using var process = Process.GetProcessById(processId);
            foreach (var loadedPath in FindLoadedBridgePaths(process))
            {
                if (!LoadedBridgeMatchesPort(loadedPath, port))
                    continue;
                bridgePath = loadedPath;
                var restoredProgressPath = ReadSidecarPath(loadedPath + ".progress.path");
                if (!string.IsNullOrWhiteSpace(restoredProgressPath))
                    progressPath = restoredProgressPath;
                else
                    progressPath = loadedPath + ".progress.json";
                return;
            }
        }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
        }
    }

    private static bool LoadedBridgeMatchesPort(string loadedPath, int port)
    {
        var sidecarPort = ReadSidecarPath(loadedPath + ".port");
        if (int.TryParse(sidecarPort, out var parsedPort))
            return parsedPort == port;
        return Path.GetFileNameWithoutExtension(loadedPath).EndsWith("-" + port, StringComparison.OrdinalIgnoreCase);
    }

    private static string ReadSidecarPath(string path)
    {
        try
        {
            return File.Exists(path) ? File.ReadAllText(path).Trim() : "";
        }
        catch (IOException)
        {
            return "";
        }
        catch (UnauthorizedAccessException)
        {
            return "";
        }
    }

    private static IEnumerable<string> SplitLines(string value) =>
        value.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)
            .Select(line => line.Trim())
            .Where(line => line.Length > 0);

    private string ReadListenerStatus()
    {
        try
        {
            var path = bridgePath + ".listen.json";
            return File.Exists(path) ? File.ReadAllText(path).Trim() : "";
        }
        catch (IOException)
        {
            return "";
        }
    }

    private static List<string> FindLoadedBridgePaths(Process process)
    {
        var paths = new List<string>();
        try
        {
            foreach (ProcessModule module in process.Modules)
            {
                var file = module.FileName;
                if (Path.GetFileName(file).Contains("runtime-bridge", StringComparison.OrdinalIgnoreCase))
                    paths.Add(file);
            }
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
        }
        return paths;
    }

    private static bool SamePath(string left, string right)
    {
        try
        {
            return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return string.Equals(left, right, StringComparison.OrdinalIgnoreCase);
        }
    }

    private static string ClassifyInjectionFailure(int exitCode, string output)
    {
        var lower = output.ToLowerInvariant();
        if (exitCode is 2 or 5 || lower.Contains("access denied") || lower.Contains("access is denied") || lower.Contains("win32=5"))
            return "MC-INJ-101";
        return "MC-INJ-130";
    }

    private static string Quote(string value) => "\"" + value.Replace("\"", "\\\"") + "\"";
}
