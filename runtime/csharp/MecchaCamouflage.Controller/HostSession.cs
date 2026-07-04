using System.Diagnostics;
using System.Text.Json;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Controller;

public sealed class HostSession
{
    private static readonly string[] ResetKeys =
    [
        "paint.brushSizeTexels",
        "paint.coverageStepTexels",
        "paint.serverBatchLimit",
        "paint.strokeDelayMs",
        "paint.autoMaterial",
        "paint.metallic",
        "paint.roughness",
        "paint.frontRegionMode",
        "paint.sideRegionMode",
        "paint.backRegionMode",
        "paint.fillColor",
        "paint.fillMetallic",
        "paint.fillRoughness",
        "app.processName",
        "app.alwaysOnTop",
        "app.opacity",
        "app.themeColor",
        "app.startHotkey",
        "app.previewHotkey",
        "app.unpreviewHotkey",
        "app.stopHotkey"
    ];

    public HostSession(string version)
    {
        Paths = new AppPaths(version);
        Store = new SettingsStore(Paths);
        Settings = Store.Load();
        Log = new RuntimeLog(Paths);
        Runtime = new RuntimeBridgeService(Paths, Log);
        Log.Info("App started.");
    }

    public LocalizationCatalog Localization { get; } = LocalizationCatalog.Load();
    public AppPaths Paths { get; }
    public SettingsStore Store { get; }
    public RuntimeLog Log { get; }
    public RuntimeBridgeService Runtime { get; }
    public AppSettings Settings { get; private set; }
    public bool PaintRunning { get; private set; }

    public async Task<UiSnapshot> GetSnapshotAsync(CancellationToken cancellationToken = default)
    {
        var process = Runtime.FindGameProcess(Settings.GameProcessName);
        var ping = await Runtime.PingAsync(cancellationToken);
        var progress = ReadProgressSnapshot(Runtime.ProgressPath);
        return CreateSnapshot(
            process is null ? "waiting" : "attached",
            ping.Ok && ping.Success ? "ready" : "waiting",
            ping.Ok && ping.Success ? "ready" : "stopped",
            progress);
    }

    public HostCommandResult UpdateSetting(string key, JsonElement value)
    {
        return UpdateSettings([new SettingChange(key, value)]);
    }

    public HostCommandResult UpdateSettings(IEnumerable<SettingChange> changes)
    {
        var previous = Clone(Settings);
        try
        {
            var next = Clone(Settings);
            foreach (var change in changes)
                ApplySetting(next, change.Key, change.Value);
            return CommitSettings(next, previous);
        }
        catch (Exception ex)
        {
            Settings = previous;
            return new HostCommandResult(false, ex.Message);
        }
    }

    public HostCommandResult ResetSetting(string key)
    {
        var previous = Clone(Settings);
        try
        {
            var next = Clone(Settings);
            ResetOne(next, new AppSettings(), key);
            return CommitSettings(next, previous);
        }
        catch (Exception ex)
        {
            Settings = previous;
            return new HostCommandResult(false, ex.Message);
        }
    }

    public HostCommandResult ResetSection(string section)
    {
        var previous = Clone(Settings);
        var next = Clone(Settings);
        var defaults = new AppSettings();
        switch (section.Trim().ToLowerInvariant())
        {
            case "runtime":
                return new HostCommandResult(true);
            case "paint.geometry":
            case "geometry":
                next.Paint.StrokeSizeTexels = defaults.Paint.StrokeSizeTexels;
                next.Paint.CoverageStepTexels = defaults.Paint.StrokeSizeTexels;
                next.Paint.ServerBatchLimit = defaults.Paint.ServerBatchLimit;
                next.Paint.ServerBatchDelayMs = defaults.Paint.ServerBatchDelayMs;
                break;
            case "paint.material":
            case "material":
                next.Paint.AutoMaterial = defaults.Paint.AutoMaterial;
                next.Paint.Metallic = defaults.Paint.Metallic;
                next.Paint.Roughness = defaults.Paint.Roughness;
                break;
            case "regions":
                next.Paint.FrontRegionMode = defaults.Paint.FrontRegionMode;
                next.Paint.SideRegionMode = defaults.Paint.SideRegionMode;
                next.Paint.BackRegionMode = defaults.Paint.BackRegionMode;
                break;
            case "fill":
            case "fill.material":
                next.Paint.FillColor = defaults.Paint.FillColor;
                next.Paint.FillMetallic = defaults.Paint.FillMetallic;
                next.Paint.FillRoughness = defaults.Paint.FillRoughness;
                break;
            case "app":
                next.GameProcessName = defaults.GameProcessName;
                next.AlwaysOnTop = defaults.AlwaysOnTop;
                next.Opacity = defaults.Opacity;
                next.ThemeColor = defaults.ThemeColor;
                next.StartHotkey = defaults.StartHotkey;
                next.PreviewHotkey = defaults.PreviewHotkey;
                next.UnPreviewHotkey = defaults.UnPreviewHotkey;
                next.StopHotkey = defaults.StopHotkey;
                break;
            default:
                return new HostCommandResult(false, $"Unknown section: {section}");
        }
        return CommitSettings(next, previous);
    }

    public HostCommandResult ResetAllSettings()
    {
        var defaults = new AppSettings
        {
            Language = Settings.Language,
            PanelX = Settings.PanelX,
            PanelY = Settings.PanelY,
            PanelWidth = Settings.PanelWidth,
            PanelHeight = Settings.PanelHeight
        };
        Settings = defaults;
        Store.Save(Settings);
        return new HostCommandResult(true);
    }

    public void SetWindowSnapshot(double width, double height, double x, double y)
    {
        if (width > 0)
            Settings.PanelWidth = width;
        if (height > 0)
            Settings.PanelHeight = height;
        Settings.PanelX = x;
        Settings.PanelY = y;
        Settings = SettingsStore.Clamp(Settings);
        Store.Save(Settings);
    }

    public async Task<HostCommandResult> RunPaintAsync(bool previewOnly, bool unpreviewOnly, CancellationToken cancellationToken = default)
    {
        if (PaintRunning)
            return new HostCommandResult(false, "Paint is already running.");
        PaintRunning = true;
        try
        {
            var ready = await Runtime.EnsureReadyAsync(Settings.GameProcessName, cancellationToken);
            if (!ready)
                return new HostCommandResult(false, "Bridge is not ready.");
            var process = Runtime.FindGameProcess(Settings.GameProcessName);
            if (process is null)
            {
                Log.Warn("Game process not found.");
                return new HostCommandResult(false, "Game process not found.");
            }
            Log.Info(previewOnly ? "Preview started." : (unpreviewOnly ? "UnPreview started." : "Paint started."));
            var payload = BridgePayloadBuilder.BuildPaintPayload(
                Settings,
                process.Id,
                Settings.GameProcessName,
                new PaintRequestOptions(previewOnly, unpreviewOnly, Environment.GetEnvironmentVariable("MECCHA_RESEARCH_ARTIFACTS") == "1"));
            var response = await Runtime.SendPaintAsync(payload, cancellationToken);
            var message = FriendlyBridgeMessage(response.Message.Length > 0 ? response.Message : response.Stage);
            if (response.Success)
            {
                Log.Info(message);
                return new HostCommandResult(true, message);
            }
            Log.Error(message);
            return new HostCommandResult(false, message);
        }
        finally
        {
            PaintRunning = false;
        }
    }

    public async Task<HostCommandResult> StopPaintAsync(CancellationToken cancellationToken = default)
    {
        var response = await Runtime.CancelPaintAsync(cancellationToken);
        var message = FriendlyBridgeMessage(response.Message.Length > 0 ? response.Message : response.Stage);
        Log.Info(response.Success ? "Paint canceled." : "Paint cancel failed: " + message);
        PaintRunning = false;
        return new HostCommandResult(response.Success, response.Success ? "Paint canceled." : message);
    }

    public void OpenLogs()
    {
        Directory.CreateDirectory(Paths.LogDirectory);
        Process.Start(new ProcessStartInfo(Paths.LogDirectory) { UseShellExecute = true });
    }

    public async Task ShutdownBridgeAsync()
    {
        var response = await Runtime.PingAsync();
        if (response.Ok)
            _ = await Runtime.ShutdownAsync();
    }

    private HostCommandResult CommitSettings(AppSettings next, AppSettings previous)
    {
        next = SettingsStore.Clamp(next);
        var hotkeys = HotkeySet.From(next);
        if (!hotkeys.TryValidate(out var message))
        {
            Settings = previous;
            return new HostCommandResult(false, message);
        }
        Settings = next;
        Store.Save(Settings);
        return new HostCommandResult(true);
    }

    public static string FriendlyBridgeMessage(string message)
    {
        if (string.IsNullOrWhiteSpace(message))
            return "";

        var value = message.Trim();
        var lower = value.ToLowerInvariant();

        if (lower.Contains("already running"))
            return "Paint is already running.";
        if (lower is "mesh_first_paint_done" ||
            lower.Contains("mesh-first paint completed") ||
            lower.Contains("sent through serverpaintbatch"))
            return "Paint completed.";
        if (lower.Contains("mesh-first paint cancelled") || lower.Contains("mesh-first paint canceled"))
            return "Paint canceled.";
        if (lower is "mesh_preview_done" || lower.Contains("local preview material texture imported"))
            return "Preview applied.";
        if (lower is "mesh_unpreview_done" || lower.Contains("local preview material texture restored"))
            return "Preview restored.";
        if (lower is "mesh_preview_failed" || lower.Contains("local preview material texture import failed"))
            return "Preview failed.";
        if (lower is "mesh_unpreview_failed" || lower.Contains("local preview material restore failed"))
            return "Preview restore failed.";
        if (lower is "mesh_unpreview_snapshot_unavailable")
            return "No preview is available to restore.";
        if (lower is "mesh_unpreview_component_mismatch")
            return "The saved preview belongs to a different mesh.";
        if (lower is "mesh_local_visual_sync_failed")
            return "Paint completed, but local preview failed.";
        if (lower is "mesh_server_batch_failed")
            return "Paint failed while sending strokes.";

        return value
            .Replace("mesh-first ", "", StringComparison.OrdinalIgnoreCase)
            .Replace("mesh first ", "", StringComparison.OrdinalIgnoreCase)
            .Replace("mesh_first_", "", StringComparison.OrdinalIgnoreCase);
    }

    private static AppSettings Clone(AppSettings source) =>
        JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(source)) ?? new AppSettings();

    private UiSnapshot CreateSnapshot(string process, string bridge, string service, ProgressSnapshot? progress)
    {
        var defaults = new AppSettings();
        var percent = 0.0;
        var eta = "-";
        var elapsed = "-";
        if (progress is not null)
        {
            percent = progress.TotalSteps > 0
                ? Math.Clamp(progress.Step * 100.0 / progress.TotalSteps, 0.0, 100.0)
                : Math.Clamp(progress.Progress * 100.0, 0.0, 100.0);
            eta = FormatDuration(progress.PaintEtaMs);
            elapsed = FormatDuration(progress.PaintElapsedMs);
        }

        return new UiSnapshot(
            VersionInfo.Current,
            Settings.Language,
            new RuntimeSnapshot(process, bridge, service, percent, eta, elapsed, Log.Text, PaintRunning),
            ToSnapshot(Settings),
            ToSnapshot(defaults),
            BuildResetSnapshot(Settings, defaults),
            LocalizationCatalog.SupportedLocales.Select(locale => new LocaleSnapshot(locale.Code, locale.NativeName)).ToArray(),
            Localization.All);
    }

    private static SettingsSnapshot ToSnapshot(AppSettings settings)
    {
        var paint = settings.Paint;
        return new SettingsSnapshot(
            new PaintSnapshot(
                paint.StrokeSizeTexels,
                paint.CoverageStepTexels,
                paint.ServerBatchDelayMs,
                paint.ServerBatchLimit,
                paint.AutoMaterial,
                paint.Metallic,
                paint.Roughness,
                SettingsStore.RegionModeText(paint.FrontRegionMode),
                SettingsStore.RegionModeText(paint.SideRegionMode),
                SettingsStore.RegionModeText(paint.BackRegionMode),
                paint.FillColor.ToHex(),
                paint.FillMetallic,
                paint.FillRoughness,
                paint.UsesFill),
            new AppSnapshot(
                settings.GameProcessName,
                settings.AlwaysOnTop,
                settings.Opacity,
                settings.ThemeColor.ToHex(),
                settings.StartHotkey,
                settings.PreviewHotkey,
                settings.UnPreviewHotkey,
                settings.StopHotkey));
    }

    private static ResetSnapshot BuildResetSnapshot(AppSettings settings, AppSettings defaults)
    {
        var map = ResetKeys.ToDictionary(key => key, key => !SettingEquals(settings, defaults, key), StringComparer.OrdinalIgnoreCase);
        var sections = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase)
        {
            ["paint.geometry"] = map["paint.brushSizeTexels"] || map["paint.coverageStepTexels"] ||
                                  map["paint.serverBatchLimit"] || map["paint.strokeDelayMs"],
            ["paint.material"] = map["paint.autoMaterial"] || map["paint.metallic"] || map["paint.roughness"],
            ["regions"] = map["paint.frontRegionMode"] || map["paint.sideRegionMode"] || map["paint.backRegionMode"],
            ["fill.material"] = map["paint.fillColor"] || map["paint.fillMetallic"] || map["paint.fillRoughness"],
            ["app"] = map["app.processName"] || map["app.alwaysOnTop"] || map["app.opacity"] || map["app.themeColor"] ||
                    map["app.startHotkey"] || map["app.previewHotkey"] || map["app.unpreviewHotkey"] || map["app.stopHotkey"]
        };
        return new ResetSnapshot(map, sections);
    }

    private static bool SettingEquals(AppSettings left, AppSettings right, string key) => key switch
    {
        "paint.brushSizeTexels" => Nearly(left.Paint.StrokeSizeTexels, right.Paint.StrokeSizeTexels),
        "paint.coverageStepTexels" => Nearly(left.Paint.CoverageStepTexels, right.Paint.CoverageStepTexels),
        "paint.serverBatchLimit" => left.Paint.ServerBatchLimit == right.Paint.ServerBatchLimit,
        "paint.strokeDelayMs" => left.Paint.ServerBatchDelayMs == right.Paint.ServerBatchDelayMs,
        "paint.autoMaterial" => left.Paint.AutoMaterial == right.Paint.AutoMaterial,
        "paint.metallic" => Nearly(left.Paint.Metallic, right.Paint.Metallic),
        "paint.roughness" => Nearly(left.Paint.Roughness, right.Paint.Roughness),
        "paint.frontRegionMode" => left.Paint.FrontRegionMode == right.Paint.FrontRegionMode,
        "paint.sideRegionMode" => left.Paint.SideRegionMode == right.Paint.SideRegionMode,
        "paint.backRegionMode" => left.Paint.BackRegionMode == right.Paint.BackRegionMode,
        "paint.fillColor" => left.Paint.FillColor == right.Paint.FillColor,
        "paint.fillMetallic" => Nearly(left.Paint.FillMetallic, right.Paint.FillMetallic),
        "paint.fillRoughness" => Nearly(left.Paint.FillRoughness, right.Paint.FillRoughness),
        "app.processName" => left.GameProcessName == right.GameProcessName,
        "app.alwaysOnTop" => left.AlwaysOnTop == right.AlwaysOnTop,
        "app.opacity" => Nearly(left.Opacity, right.Opacity),
        "app.themeColor" => left.ThemeColor == right.ThemeColor,
        "app.startHotkey" => left.StartHotkey == right.StartHotkey,
        "app.previewHotkey" => left.PreviewHotkey == right.PreviewHotkey,
        "app.unpreviewHotkey" => left.UnPreviewHotkey == right.UnPreviewHotkey,
        "app.stopHotkey" => left.StopHotkey == right.StopHotkey,
        _ => true
    };

    private static void ResetOne(AppSettings settings, AppSettings defaults, string key)
    {
        switch (key)
        {
            case "paint.brushSizeTexels":
            case "paint.coverageStepTexels":
                settings.Paint.StrokeSizeTexels = defaults.Paint.StrokeSizeTexels;
                settings.Paint.CoverageStepTexels = defaults.Paint.StrokeSizeTexels;
                break;
            case "paint.strokeDelayMs": settings.Paint.ServerBatchDelayMs = defaults.Paint.ServerBatchDelayMs; break;
            case "paint.serverBatchLimit": settings.Paint.ServerBatchLimit = defaults.Paint.ServerBatchLimit; break;
            case "paint.autoMaterial": settings.Paint.AutoMaterial = defaults.Paint.AutoMaterial; break;
            case "paint.metallic": settings.Paint.Metallic = defaults.Paint.Metallic; break;
            case "paint.roughness": settings.Paint.Roughness = defaults.Paint.Roughness; break;
            case "paint.frontRegionMode": settings.Paint.FrontRegionMode = defaults.Paint.FrontRegionMode; break;
            case "paint.sideRegionMode": settings.Paint.SideRegionMode = defaults.Paint.SideRegionMode; break;
            case "paint.backRegionMode": settings.Paint.BackRegionMode = defaults.Paint.BackRegionMode; break;
            case "paint.fillColor": settings.Paint.FillColor = defaults.Paint.FillColor; break;
            case "paint.fillMetallic": settings.Paint.FillMetallic = defaults.Paint.FillMetallic; break;
            case "paint.fillRoughness": settings.Paint.FillRoughness = defaults.Paint.FillRoughness; break;
            case "app.processName": settings.GameProcessName = defaults.GameProcessName; break;
            case "app.alwaysOnTop": settings.AlwaysOnTop = defaults.AlwaysOnTop; break;
            case "app.opacity": settings.Opacity = defaults.Opacity; break;
            case "app.themeColor": settings.ThemeColor = defaults.ThemeColor; break;
            case "app.startHotkey": settings.StartHotkey = defaults.StartHotkey; break;
            case "app.previewHotkey": settings.PreviewHotkey = defaults.PreviewHotkey; break;
            case "app.unpreviewHotkey": settings.UnPreviewHotkey = defaults.UnPreviewHotkey; break;
            case "app.stopHotkey": settings.StopHotkey = defaults.StopHotkey; break;
            default: throw new ArgumentException($"Unknown setting: {key}");
        }
    }

    private static void ApplySetting(AppSettings settings, string key, JsonElement value)
    {
        switch (key)
        {
            case "paint.brushSizeTexels":
            case "paint.coverageStepTexels":
                settings.Paint.StrokeSizeTexels = value.GetDouble();
                settings.Paint.CoverageStepTexels = settings.Paint.StrokeSizeTexels;
                break;
            case "paint.strokeDelayMs": settings.Paint.ServerBatchDelayMs = value.GetInt32(); break;
            case "paint.serverBatchLimit": settings.Paint.ServerBatchLimit = value.GetInt32(); break;
            case "paint.autoMaterial": settings.Paint.AutoMaterial = value.GetBoolean(); break;
            case "paint.metallic": settings.Paint.Metallic = value.GetDouble(); break;
            case "paint.roughness": settings.Paint.Roughness = value.GetDouble(); break;
            case "paint.frontRegionMode": settings.Paint.FrontRegionMode = ParseRegionMode(value.GetString()); break;
            case "paint.sideRegionMode": settings.Paint.SideRegionMode = ParseRegionMode(value.GetString()); break;
            case "paint.backRegionMode": settings.Paint.BackRegionMode = ParseRegionMode(value.GetString()); break;
            case "paint.fillColor":
                if (!RgbColor.TryParse(value.GetString(), out var fill))
                    throw new ArgumentException("Fill color must be #RRGGBB.");
                settings.Paint.FillColor = fill;
                break;
            case "paint.fillMetallic": settings.Paint.FillMetallic = value.GetDouble(); break;
            case "paint.fillRoughness": settings.Paint.FillRoughness = value.GetDouble(); break;
            case "app.language": settings.Language = value.GetString() ?? settings.Language; break;
            case "app.processName": settings.GameProcessName = value.GetString() ?? settings.GameProcessName; break;
            case "app.alwaysOnTop": settings.AlwaysOnTop = value.GetBoolean(); break;
            case "app.opacity": settings.Opacity = value.GetDouble(); break;
            case "app.themeColor":
                if (!RgbColor.TryParse(value.GetString(), out var theme))
                    throw new ArgumentException("Theme color must be #RRGGBB.");
                settings.ThemeColor = theme;
                break;
            case "app.startHotkey": settings.StartHotkey = HotkeySet.Normalize(value.GetString()); break;
            case "app.previewHotkey": settings.PreviewHotkey = HotkeySet.Normalize(value.GetString()); break;
            case "app.unpreviewHotkey": settings.UnPreviewHotkey = HotkeySet.Normalize(value.GetString()); break;
            case "app.stopHotkey": settings.StopHotkey = HotkeySet.Normalize(value.GetString()); break;
            default: throw new ArgumentException($"Unknown setting: {key}");
        }
    }

    private static RegionMode ParseRegionMode(string? value)
    {
        if (Enum.TryParse<RegionMode>(value, true, out var mode) && Enum.IsDefined(mode))
            return mode;
        throw new ArgumentException("Region mode must be paint, fill, or skip.");
    }

    private static ProgressSnapshot? ReadProgressSnapshot(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            return null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var root = doc.RootElement;
            return new ProgressSnapshot(
                Text(root, "phase", Text(root, "stage", "")),
                Text(root, "result", ""),
                Bool(root, "terminal", false),
                Int(root, "step", 0),
                Int(root, "total_steps", Int(root, "total_strokes", 0)),
                Number(root, "progress", 0.0),
                Number(root, "paint_eta_ms", -1.0),
                Number(root, "paint_elapsed_ms", Number(root, "elapsed_ms", -1.0)));
        }
        catch
        {
            return null;
        }
    }

    private static string FormatDuration(double milliseconds)
    {
        if (!double.IsFinite(milliseconds) || milliseconds < 0.0)
            return "-";
        if (milliseconds < 1000.0)
            return "<1s";
        var totalSeconds = (int)Math.Round(milliseconds / 1000.0);
        if (totalSeconds < 60)
            return totalSeconds + "s";
        var minutes = totalSeconds / 60;
        var seconds = totalSeconds % 60;
        if (minutes < 60)
            return $"{minutes}m {seconds:00}s";
        var hours = minutes / 60;
        minutes %= 60;
        return $"{hours}h {minutes:00}m";
    }

    private static string Text(JsonElement root, string key, string fallback) =>
        root.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? fallback : fallback;

    private static bool Bool(JsonElement root, string key, bool fallback) =>
        root.TryGetProperty(key, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False ? value.GetBoolean() : fallback;

    private static int Int(JsonElement root, string key, int fallback) =>
        root.TryGetProperty(key, out var value) && value.TryGetInt32(out var parsed) ? parsed : fallback;

    private static double Number(JsonElement root, string key, double fallback) =>
        root.TryGetProperty(key, out var value) && value.TryGetDouble(out var parsed) ? parsed : fallback;

    private static bool Nearly(double left, double right) => Math.Abs(left - right) < 0.000001;
}
