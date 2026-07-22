using System.Diagnostics;
using System.Text.Json;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Controller;

public sealed class HostSession
{
    private const int NativeCancelAdmissionRetryAttempts = 40;
    private static readonly TimeSpan NativeCancelAdmissionRetryDelay = TimeSpan.FromMilliseconds(25);

    private static readonly string[] ResetKeys =
    [
        "paint.brushSizeTexels",
        "paint.colorCompressionTolerance",
        "paint.autoMaterial",
        "paint.metallic",
        "paint.roughness",
        "paint.emissive",
        "paint.frontRegionMode",
        "paint.sideRegionMode",
        "paint.backRegionMode",
        "paint.fillColor",
        "paint.fillMetallic",
        "paint.fillRoughness",
        "paint.fillEmissive",
        "app.processName",
        "app.alwaysOnTop",
        "app.opacity",
        "app.themeColor",
        "app.startHotkey",
        "app.previewHotkey",
        "app.unpreviewHotkey",
        "app.stopHotkey"
    ];

    private enum PaintCancelState
    {
        None,
        PreDispatchPending,
        Sending,
        AcceptedAwaitingTerminal
    }

    public HostSession(string version, int diagnosticStrokeLimit = 0)
    {
        Paths = new AppPaths(version);
        DiagnosticsState.EnsureInitialized(Paths, version);
        Store = new SettingsStore(Paths);
        Settings = Store.Load();
        Log = new RuntimeLog(Paths);
        Runtime = new RuntimeBridgeService(Paths, Log);
        this.diagnosticStrokeLimit = Math.Clamp(diagnosticStrokeLimit, 0, 10_000);
    }

    public LocalizationCatalog Localization { get; } = LocalizationCatalog.Load();
    public AppPaths Paths { get; }
    public SettingsStore Store { get; }
    public RuntimeLog Log { get; }
    public RuntimeBridgeService Runtime { get; }
    public AppSettings Settings { get; private set; }
    public bool PaintRunning { get; private set; }
    private readonly SemaphoreSlim bridgeWarmupGate = new(1, 1);
    private readonly object paintStateGate = new();
    private DateTimeOffset nextBridgeWarmupAttempt;
    private DateTimeOffset currentPaintStartedAt = DateTimeOffset.MinValue;
    private bool finalProgressLogged;
    private bool currentProgressIsServerPaint;
    private bool nativePaintMayBeRunning;
    private PaintCancelState cancelState;
    private int nextPaintGeneration;
    private int activePaintGeneration;
    private int cancelPaintGeneration;
    // This becomes non-zero only immediately before the authenticated paint request begins.
    // A stop during process/bridge attachment must latch locally instead of asking native to
    // cancel a job which does not exist yet and then allowing the late send to proceed.
    private int paintRequestDispatchGeneration;
    private readonly object replayPassLogGate = new();
    private readonly HashSet<string> loggedReplayPasses = new(StringComparer.Ordinal);
    private readonly int diagnosticStrokeLimit;

    public async Task<UiSnapshot> GetSnapshotAsync(CancellationToken cancellationToken = default)
    {
        using var process = Runtime.FindGameProcess(Settings.GameProcessName);
        var ping = await Runtime.PingAsync(cancellationToken, RuntimeBridgeService.BridgeProbeTimeout);
        var progress = ReadCurrentProgressSnapshot(liveOnly: true);
        LogReplayPassTransition(progress);
        var bridgeReady = process is not null &&
            Runtime.IsConnected &&
            ping.Ok &&
            ping.Success &&
            (ping.ProcessId is null || ping.ProcessId == process.Id);
        return CreateSnapshot(
            process is null ? "waiting" : "attached",
            bridgeReady ? "connected" : "waiting",
            bridgeReady ? "ready" : "stopped",
            progress);
    }

    public async Task WarmupBridgeAsync(CancellationToken cancellationToken = default)
    {
        var now = DateTimeOffset.UtcNow;
        if (now < nextBridgeWarmupAttempt)
            return;
        if (!await bridgeWarmupGate.WaitAsync(0, cancellationToken))
            return;
        try
        {
            using var process = Runtime.FindGameProcess(Settings.GameProcessName);
            if (process is null)
            {
                _ = await Runtime.EnsureReadyAsync(Settings.GameProcessName, cancellationToken);
                nextBridgeWarmupAttempt = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(2);
                return;
            }
            var ready = await Runtime.EnsureReadyAsync(process, cancellationToken);
            nextBridgeWarmupAttempt = ready
                ? DateTimeOffset.UtcNow + TimeSpan.FromSeconds(2)
                : DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            Log.Warn("Bridge warmup failed: " + ex.Message);
            nextBridgeWarmupAttempt = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        }
        finally
        {
            bridgeWarmupGate.Release();
        }
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
                next.Paint.BrushSizeTexels = defaults.Paint.BrushSizeTexels;
                next.Paint.ColorCompressionTolerance = defaults.Paint.ColorCompressionTolerance;
                break;
            case "paint.material":
            case "material":
                next.Paint.AutoMaterial = defaults.Paint.AutoMaterial;
                next.Paint.Metallic = defaults.Paint.Metallic;
                next.Paint.Roughness = defaults.Paint.Roughness;
                next.Paint.Emissive = defaults.Paint.Emissive;
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
                next.Paint.FillEmissive = defaults.Paint.FillEmissive;
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
        int runGeneration;
        lock (paintStateGate)
        {
            if (PaintRunning || nativePaintMayBeRunning)
            {
                const string alreadyRunning = "Paint: already running.";
                Log.Warn(alreadyRunning);
                return new HostCommandResult(false, alreadyRunning);
            }
            PaintRunning = true;
            runGeneration = ++nextPaintGeneration;
            activePaintGeneration = runGeneration;
            cancelState = PaintCancelState.None;
            cancelPaintGeneration = 0;
            paintRequestDispatchGeneration = 0;
        }
        currentPaintStartedAt = DateTimeOffset.UtcNow;
        currentProgressIsServerPaint = !previewOnly && !unpreviewOnly;
        finalProgressLogged = false;
        ResetReplayPassLog();
        TryDeleteProgressSnapshot();
        try
        {
            using var process = Runtime.FindGameProcess(Settings.GameProcessName);
            if (process is null)
            {
                Log.Warn("Game process not found.");
                return new HostCommandResult(false, "Game process not found.");
            }
            var ready = await Runtime.EnsureReadyAsync(process, cancellationToken);
            if (IsPreDispatchCancellationPending(runGeneration))
            {
                const string canceledBeforeDispatch = "Paint: canceled.";
                Log.Info(canceledBeforeDispatch);
                return new HostCommandResult(false, canceledBeforeDispatch);
            }
            if (!ready)
                return new HostCommandResult(false, "Bridge is not connected.");
            var startedMessage = previewOnly ? "Preview: started." : (unpreviewOnly ? "UnPreview: started." : "Paint: started.");
            Log.Info(startedMessage);
            var payload = BridgePayloadBuilder.BuildPaintPayload(
                Settings,
                process.Id,
                Settings.GameProcessName,
                new PaintRequestOptions(
                    PreviewOnly: previewOnly,
                    UnPreviewOnly: unpreviewOnly,
                    ResearchArtifacts: BuildFeatures.ResearchArtifactsEnabled,
                    DiagnosticStrokeLimit: diagnosticStrokeLimit));
            if (!TryBeginPaintDispatch(runGeneration))
            {
                const string canceledBeforeDispatch = "Paint: canceled.";
                Log.Info(canceledBeforeDispatch);
                return new HostCommandResult(false, canceledBeforeDispatch);
            }
            var response = await Runtime.SendPaintAsync(payload, cancellationToken);
            if (diagnosticStrokeLimit > 0)
            {
                var diagnostic = PaintDiagnosticSummary(response);
                if (!string.IsNullOrEmpty(diagnostic))
                    Log.Info(diagnostic);
            }
            var message = FriendlyBridgeMessage(response.Message.Length > 0 ? response.Message : response.Stage);
            if (response.Success)
            {
                message = DescribePaintCompletion(message, serverPaint: !previewOnly && !unpreviewOnly);
                lock (paintStateGate)
                {
                    if (activePaintGeneration == runGeneration)
                        nativePaintMayBeRunning = false;
                }
                LogFinalProgressOnce();
                Log.Info(message);
                return new HostCommandResult(true, message);
            }
            if (IsPaintCancellationMessage(message))
            {
                lock (paintStateGate)
                {
                    if (activePaintGeneration == runGeneration)
                        nativePaintMayBeRunning = false;
                }
                LogFinalProgressOnce();
                Log.Info(message);
                return new HostCommandResult(false, message);
            }
            if (IsGuardWarning(message))
            {
                lock (paintStateGate)
                {
                    if (activePaintGeneration == runGeneration)
                        nativePaintMayBeRunning = message == "Paint: already running.";
                }
                Log.Warn(message);
                return new HostCommandResult(false, message);
            }
            lock (paintStateGate)
            {
                if (activePaintGeneration == runGeneration)
                    nativePaintMayBeRunning = false;
            }
            LogFailureProgressOnce();
            Log.Error(message);
            var failureDetail = PaintFailureDetail(response);
            if (failureDetail is not null)
                Log.Warn("Paint detail: " + failureDetail);
            return new HostCommandResult(false, message);
        }
        finally
        {
            lock (paintStateGate)
            {
                if (activePaintGeneration == runGeneration)
                {
                    PaintRunning = false;
                    activePaintGeneration = 0;
                    cancelState = PaintCancelState.None;
                    cancelPaintGeneration = 0;
                    paintRequestDispatchGeneration = 0;
                }
            }
            currentProgressIsServerPaint = false;
        }
    }

    public async Task<HostCommandResult> StopPaintAsync(CancellationToken cancellationToken = default)
    {
        int requestedGeneration;
        bool controllerOwnedPaint;
        lock (paintStateGate)
        {
            if (cancelState == PaintCancelState.Sending)
                return new HostCommandResult(true, "Paint: cancel requested.");
            if (cancelState == PaintCancelState.PreDispatchPending)
                return new HostCommandResult(true, "Paint: cancel requested.");
            if (cancelState == PaintCancelState.AcceptedAwaitingTerminal)
                return new HostCommandResult(true, "Paint: cancel requested.");
            if (!PaintRunning && !nativePaintMayBeRunning)
            {
                const string noActivePaint = "Paint: no active paint to cancel.";
                Log.Warn(noActivePaint);
                return new HostCommandResult(false, noActivePaint);
            }
            controllerOwnedPaint = PaintRunning;
            requestedGeneration = controllerOwnedPaint ? activePaintGeneration : 0;
            if (controllerOwnedPaint && paintRequestDispatchGeneration != requestedGeneration)
            {
                cancelState = PaintCancelState.PreDispatchPending;
                cancelPaintGeneration = requestedGeneration;
                const string canceledBeforeDispatch = "Paint: canceled.";
                Log.Info(canceledBeforeDispatch);
                return new HostCommandResult(true, canceledBeforeDispatch);
            }
            cancelState = PaintCancelState.Sending;
            cancelPaintGeneration = requestedGeneration;
        }
        BridgeReply response;
        try
        {
            response = await Runtime.CancelPaintAsync(cancellationToken);
            for (var attempt = 0;
                 attempt < NativeCancelAdmissionRetryAttempts &&
                 ShouldRetryCancelAfterEarlyAcknowledgement(requestedGeneration, controllerOwnedPaint, response);
                 ++attempt)
            {
                await Task.Delay(NativeCancelAdmissionRetryDelay, cancellationToken);
                response = await Runtime.CancelPaintAsync(cancellationToken);
            }
        }
        catch
        {
            ClearCancelStateIfOwned(requestedGeneration);
            throw;
        }
        var message = FriendlyBridgeMessage(response.Message.Length > 0 ? response.Message : response.Stage);
        var cancelledJobs = CancelledPaintJobCount(response);
        var nativeCancellationLatched = NativePaintRequestCancellationLatched(response);
        lock (paintStateGate)
        {
            // The original command can terminalize while the independent cancel request is in
            // flight. Never revive a completed generation with a late acknowledgement.
            var sameControllerPaint = controllerOwnedPaint &&
                                      PaintRunning &&
                                      activePaintGeneration == requestedGeneration;
            var ownsCancelState = cancelState == PaintCancelState.Sending &&
                                  cancelPaintGeneration == requestedGeneration;
            if (!ownsCancelState)
                return new HostCommandResult(response.Success, "Paint: cancellation completed after the paint terminalized.");

            if (!response.Success)
            {
                cancelState = PaintCancelState.None;
                cancelPaintGeneration = 0;
                if (IsGuardWarning(message))
                    Log.Warn(message);
                else
                    Log.Error("Paint: cancel failed: " + message);
                return new HostCommandResult(false, message);
            }

            if (cancelledJobs == 0 && !nativeCancellationLatched)
            {
                cancelState = PaintCancelState.None;
                cancelPaintGeneration = 0;
                nativePaintMayBeRunning = false;
                if (sameControllerPaint && paintRequestDispatchGeneration == requestedGeneration)
                {
                    const string nativeAdmissionMissed = "Paint: cancel was not observed by the native paint request. Retry stop.";
                    Log.Warn(nativeAdmissionMissed);
                    return new HostCommandResult(false, nativeAdmissionMissed);
                }
                const string noActivePaint = "Paint: no active paint to cancel.";
                Log.Warn(noActivePaint);
                return new HostCommandResult(false, noActivePaint);
            }

            if (cancelledJobs is null)
            {
                cancelState = PaintCancelState.None;
                cancelPaintGeneration = 0;
                const string malformed = "Paint: cancel response did not include native job counts.";
                Log.Error(malformed);
                return new HostCommandResult(false, malformed);
            }

            if (!sameControllerPaint)
            {
                // A foreign native job has no controller-owned terminal request to clear this
                // state. Its acknowledgement is terminal from the UI's perspective.
                cancelState = PaintCancelState.None;
                cancelPaintGeneration = 0;
                nativePaintMayBeRunning = false;
                const string completed = "Paint: cancel requested.";
                Log.Info(completed);
                return new HostCommandResult(true, completed);
            }

            // The original request remains active only long enough to observe the bounded local
            // queue tail reaching zero.
            cancelState = PaintCancelState.AcceptedAwaitingTerminal;
            nativePaintMayBeRunning = true;
            const string pending = "Paint: cancel requested.";
            Log.Info(pending);
            return new HostCommandResult(true, pending);
        }
    }

    private bool ShouldRetryCancelAfterEarlyAcknowledgement(
        int generation,
        bool controllerOwnedPaint,
        BridgeReply response)
    {
        if (!controllerOwnedPaint ||
            !response.Success ||
            CancelledPaintJobCount(response) is not 0 ||
            NativePaintRequestCancellationLatched(response))
        {
            return false;
        }
        lock (paintStateGate)
        {
            return PaintRunning &&
                   activePaintGeneration == generation &&
                   paintRequestDispatchGeneration == generation &&
                   cancelState == PaintCancelState.Sending &&
                   cancelPaintGeneration == generation;
        }
    }

    private void ClearCancelStateIfOwned(int generation)
    {
        lock (paintStateGate)
        {
            if (cancelState == PaintCancelState.Sending && cancelPaintGeneration == generation)
            {
                cancelState = PaintCancelState.None;
                cancelPaintGeneration = 0;
            }
        }
    }

    private bool TryBeginPaintDispatch(int generation)
    {
        lock (paintStateGate)
        {
            if (!PaintRunning || activePaintGeneration != generation ||
                IsPreDispatchCancellationPendingLocked(generation))
            {
                return false;
            }
            paintRequestDispatchGeneration = generation;
            return true;
        }
    }

    private bool IsPreDispatchCancellationPending(int generation)
    {
        lock (paintStateGate)
            return IsPreDispatchCancellationPendingLocked(generation);
    }

    private bool IsPreDispatchCancellationPendingLocked(int generation) =>
        cancelState == PaintCancelState.PreDispatchPending &&
        cancelPaintGeneration == generation;

    public static int? CancelledPaintJobCount(BridgeReply response)
    {
        if (string.IsNullOrWhiteSpace(response.Raw))
            return null;
        try
        {
            using var doc = JsonDocument.Parse(response.Raw);
            if (!doc.RootElement.TryGetProperty("metadata", out var metadata) ||
                metadata.ValueKind != JsonValueKind.Object)
            {
                return null;
            }
            var active = Int(metadata, "cancelled_active_paint_jobs", 0);
            var queued = Int(metadata, "cancelled_queued_paint_jobs", 0);
            return Math.Max(0, active) + Math.Max(0, queued);
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Native reports this when cancel raced with admission: there was not yet a queued/executing
    /// job to count, but the current paint request owns a cancellation latch and will terminalize
    /// before it can dispatch work.
    /// </summary>
    public static bool NativePaintRequestCancellationLatched(BridgeReply response)
    {
        if (string.IsNullOrWhiteSpace(response.Raw))
            return false;
        try
        {
            using var doc = JsonDocument.Parse(response.Raw);
            return doc.RootElement.TryGetProperty("metadata", out var metadata) &&
                   metadata.ValueKind == JsonValueKind.Object &&
                   metadata.TryGetProperty("cancel_latched_paint_request", out var latched) &&
                   latched.ValueKind is JsonValueKind.True or JsonValueKind.False &&
                   latched.GetBoolean();
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// The friendly failure text is deliberately identical for every cause, which leaves a user
    /// report with nothing to act on. Carry the native failure fields alongside it in the log.
    /// </summary>
    public static string? PaintFailureDetail(BridgeReply response)
    {
        if (string.IsNullOrWhiteSpace(response.Raw))
            return null;
        try
        {
            using var doc = JsonDocument.Parse(response.Raw);
            if (!doc.RootElement.TryGetProperty("metadata", out var metadata) ||
                metadata.ValueKind != JsonValueKind.Object)
            {
                return null;
            }
            string[] fields =
            [
                "local_visual_sync_failure"
            ];
            var parts = new List<string>();
            foreach (var field in fields)
            {
                if (!metadata.TryGetProperty(field, out var value) || value.ValueKind != JsonValueKind.String)
                    continue;
                var text = value.GetString();
                if (string.IsNullOrWhiteSpace(text))
                    continue;
                parts.Add(field + "=" + text);
            }
            return parts.Count == 0 ? null : string.Join(" | ", parts);
        }
        catch
        {
            return null;
        }
    }

    public static string? PaintDiagnosticSummary(BridgeReply response)
    {
        if (string.IsNullOrWhiteSpace(response.Raw))
            return null;
        try
        {
            using var document = JsonDocument.Parse(response.Raw);
            if (!document.RootElement.TryGetProperty("metadata", out var metadata) ||
                metadata.ValueKind != JsonValueKind.Object)
            {
                return null;
            }
            var values = new List<string>();
            foreach (var name in new[]
                     {
                         "diagnostic_strokes_before_limit",
                         "diagnostic_strokes_after_limit",
                         "local_stroke_calls",
                         "local_stroke_success",
                         "local_stroke_failures",
                         "native_queue_recorded_last_strokes",
                         "native_queue_component_last_strokes",
                         "native_queue_component_peak_strokes",
                         "native_queue_waits",
                         "first_stroke_target_channel",
                         "first_stroke_metallic",
                         "first_stroke_roughness",
                         "first_stroke_emissive"
                     })
            {
                if (metadata.TryGetProperty(name, out var value) &&
                    value.ValueKind is JsonValueKind.Number or JsonValueKind.String)
                {
                    values.Add(name + "=" + value.ToString());
                }
            }
            return values.Count == 0 ? null : "Paint diagnostic: " + string.Join(" | ", values);
        }
        catch
        {
            return null;
        }
    }

    public void OpenLogs()
    {
        Directory.CreateDirectory(Paths.DiagnosticsDirectory);
        Process.Start(new ProcessStartInfo(Paths.VersionRoot) { UseShellExecute = true });
    }

    public string ClipboardLogText()
    {
        if (!currentProgressIsServerPaint)
            return Log.Text;
        var progress = ReadCurrentProgressSnapshot(liveOnly: false);
        if (progress is null)
            return Log.Text;
        var line = FormatProgressLogLine(progress);
        if (line.Length == 0)
            return Log.Text;
        return string.IsNullOrWhiteSpace(Log.Text)
            ? line
            : Log.Text.TrimEnd() + Environment.NewLine + line;
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
            return "Paint: already running.";
        if (lower is "mesh_first_paint_done" ||
            lower is "paint completed." ||
            lower is "paint completed" ||
            lower is "paint: completed." ||
            lower is "paint: completed" ||
            lower.Contains("mesh-first paint completed") ||
            lower.Contains("sent through serverpaintbatch"))
            return "Paint: completed.";
        if (lower.Contains("paint cancellation") ||
            lower.Contains("paint canceled") ||
            lower.Contains("mesh-first paint cancelled") ||
            lower.Contains("mesh-first paint canceled"))
            return "Paint: canceled.";
        if (lower is "mesh_preview_done" || lower.Contains("local preview material texture imported"))
            return "Preview: applied.";
        if (lower is "mesh_unpreview_done" || lower.Contains("local preview material texture restored"))
            return "Preview: restored.";
        if (lower is "mesh_preview_failed" || lower.Contains("local preview material texture import failed"))
            return "Preview: failed.";
        if (lower is "mesh_unpreview_failed" || lower.Contains("local preview material restore failed"))
            return "Preview: restore failed.";
        if (lower is "mesh_unpreview_snapshot_unavailable" || lower.Contains("no local preview snapshot is available"))
            return "Preview: no active preview to restore.";
        if (lower is "mesh_unpreview_component_mismatch")
            return "The saved preview belongs to a different mesh.";
        if (lower.Contains("strokes were submitted, but local rendering failed"))
            return "Paint: strokes were sent, but local rendering failed. Do not retry automatically.";
        if (lower is "mesh_local_visual_sync_failed")
            return "Paint: strokes were sent, but local rendering failed. Do not retry automatically.";
        if (lower is "mesh_paint_context_changed" || lower.Contains("paint_context_changed"))
            return "Paint: stopped because the game paint component changed.";
        if (lower.Contains("game paint component is no longer available"))
            return "Paint: stopped because the game paint component is no longer available.";
        if (lower.Contains("local pawn is no longer available"))
            return "Paint: stopped because the local pawn is no longer available.";
        if (lower.Contains("local pawn changed"))
            return "Paint: stopped because the local pawn changed.";
        if (lower.Contains("paint_component_unavailable"))
            return "Paint: stopped because the game paint component is unavailable.";
        if (lower.Contains("unsafe color-transfer candidates"))
            return "Paint: blocked because the current mesh sampling was unsafe.";
        if (lower is "mesh_native_paint_unavailable" || lower.Contains("paintatuvwithbrush is unavailable"))
            return "Paint: the game-native paint route is unavailable.";

        return value
            .Replace("mesh-first ", "", StringComparison.OrdinalIgnoreCase)
            .Replace("mesh first ", "", StringComparison.OrdinalIgnoreCase)
            .Replace("mesh_first_", "", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// A server-paint terminal reply proves the host's local queue reached its terminal state;
    /// it cannot prove a joining client's game-owned receiver has presented its final pixels.
    /// </summary>
    public static string DescribePaintCompletion(string message, bool serverPaint) =>
        serverPaint && string.Equals(message, "Paint: completed.", StringComparison.Ordinal)
            ? "Paint: completed locally; other clients may still be rendering."
            : message;

    private static bool IsPaintCancellationMessage(string message) =>
        message == "Paint: canceled.";

    private static bool IsGuardWarning(string message) =>
        message.Equals("Paint: already running.", StringComparison.OrdinalIgnoreCase) ||
        message.Equals("Paint: no active paint to cancel.", StringComparison.OrdinalIgnoreCase) ||
        message.Equals("Preview: no active preview to restore.", StringComparison.OrdinalIgnoreCase);

    private static AppSettings Clone(AppSettings source) =>
        JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(source)) ?? new AppSettings();

    private UiSnapshot CreateSnapshot(string process, string bridge, string service, ProgressSnapshot? progress)
    {
        var defaults = new AppSettings();
        var percent = 0.0;
        var progressSource = "";
        var pass = "-";
        var passProgress = "-";
        var passEta = "-";
        var eta = "-";
        var elapsed = "-";
        if (progress is not null)
        {
            percent = progress.TotalSteps > 0
                ? Math.Clamp(progress.Step * 100.0 / progress.TotalSteps, 0.0, 100.0)
                : Math.Clamp(progress.Progress * 100.0, 0.0, 100.0);
            progressSource = progress.ReplayProgressSource;
            pass = ReplayPassLabel(progress.ReplayCurrentPass);
            passProgress = FormatReplayPassProgress(progress);
            passEta = FormatDuration(progress.ReplayCurrentPassEtaMs);
            eta = FormatEta(progress);
            elapsed = FormatDuration(progress.PaintElapsedMs);
        }

        return new UiSnapshot(
            VersionInfo.Current,
            Settings.Language,
            new RuntimeSnapshot(process, bridge, service, percent, progressSource, pass, passProgress, passEta, eta, elapsed, Log.Text, PaintRunning, progress is not null, DiagnosticsState.Snapshot(Paths)),
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
                paint.BrushSizeTexels,
                paint.AutoMaterial,
                paint.Metallic,
                paint.Roughness,
                paint.Emissive,
                SettingsStore.RegionModeText(paint.FrontRegionMode),
                SettingsStore.RegionModeText(paint.SideRegionMode),
                SettingsStore.RegionModeText(paint.BackRegionMode),
                paint.FillColor.ToHex(),
                paint.FillMetallic,
                paint.FillRoughness,
                paint.FillEmissive,
                paint.UsesFill,
                paint.ColorCompressionTolerance),
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
            ["paint.geometry"] = map["paint.brushSizeTexels"] || map["paint.colorCompressionTolerance"],
            ["paint.material"] = map["paint.autoMaterial"] || map["paint.metallic"] || map["paint.roughness"] || map["paint.emissive"],
            ["regions"] = map["paint.frontRegionMode"] || map["paint.sideRegionMode"] || map["paint.backRegionMode"],
            ["fill.material"] = map["paint.fillColor"] || map["paint.fillMetallic"] || map["paint.fillRoughness"] || map["paint.fillEmissive"],
            ["app"] = map["app.processName"] || map["app.alwaysOnTop"] || map["app.opacity"] || map["app.themeColor"] ||
                    map["app.startHotkey"] || map["app.previewHotkey"] || map["app.unpreviewHotkey"] || map["app.stopHotkey"]
        };
        return new ResetSnapshot(map, sections);
    }

    private static bool SettingEquals(AppSettings left, AppSettings right, string key) => key switch
    {
        "paint.brushSizeTexels" => Nearly(left.Paint.BrushSizeTexels, right.Paint.BrushSizeTexels),
        "paint.autoMaterial" => left.Paint.AutoMaterial == right.Paint.AutoMaterial,
        "paint.metallic" => Nearly(left.Paint.Metallic, right.Paint.Metallic),
        "paint.roughness" => Nearly(left.Paint.Roughness, right.Paint.Roughness),
        "paint.emissive" => Nearly(left.Paint.Emissive, right.Paint.Emissive),
        "paint.frontRegionMode" => left.Paint.FrontRegionMode == right.Paint.FrontRegionMode,
        "paint.sideRegionMode" => left.Paint.SideRegionMode == right.Paint.SideRegionMode,
        "paint.backRegionMode" => left.Paint.BackRegionMode == right.Paint.BackRegionMode,
        "paint.fillColor" => left.Paint.FillColor == right.Paint.FillColor,
        "paint.fillMetallic" => Nearly(left.Paint.FillMetallic, right.Paint.FillMetallic),
        "paint.fillRoughness" => Nearly(left.Paint.FillRoughness, right.Paint.FillRoughness),
        "paint.fillEmissive" => Nearly(left.Paint.FillEmissive, right.Paint.FillEmissive),
        "paint.colorCompressionTolerance" => Nearly(left.Paint.ColorCompressionTolerance, right.Paint.ColorCompressionTolerance),
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
            case "paint.brushSizeTexels": settings.Paint.BrushSizeTexels = defaults.Paint.BrushSizeTexels; break;
            case "paint.autoMaterial": settings.Paint.AutoMaterial = defaults.Paint.AutoMaterial; break;
            case "paint.metallic": settings.Paint.Metallic = defaults.Paint.Metallic; break;
            case "paint.roughness": settings.Paint.Roughness = defaults.Paint.Roughness; break;
            case "paint.emissive": settings.Paint.Emissive = defaults.Paint.Emissive; break;
            case "paint.frontRegionMode": settings.Paint.FrontRegionMode = defaults.Paint.FrontRegionMode; break;
            case "paint.sideRegionMode": settings.Paint.SideRegionMode = defaults.Paint.SideRegionMode; break;
            case "paint.backRegionMode": settings.Paint.BackRegionMode = defaults.Paint.BackRegionMode; break;
            case "paint.fillColor": settings.Paint.FillColor = defaults.Paint.FillColor; break;
            case "paint.fillMetallic": settings.Paint.FillMetallic = defaults.Paint.FillMetallic; break;
            case "paint.fillRoughness": settings.Paint.FillRoughness = defaults.Paint.FillRoughness; break;
            case "paint.fillEmissive": settings.Paint.FillEmissive = defaults.Paint.FillEmissive; break;
            case "paint.colorCompressionTolerance": settings.Paint.ColorCompressionTolerance = defaults.Paint.ColorCompressionTolerance; break;
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
            case "paint.brushSizeTexels": settings.Paint.BrushSizeTexels = value.GetDouble(); break;
            case "paint.autoMaterial": settings.Paint.AutoMaterial = value.GetBoolean(); break;
            case "paint.metallic": settings.Paint.Metallic = value.GetDouble(); break;
            case "paint.roughness": settings.Paint.Roughness = value.GetDouble(); break;
            case "paint.emissive": settings.Paint.Emissive = value.GetDouble(); break;
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
            case "paint.fillEmissive": settings.Paint.FillEmissive = value.GetDouble(); break;
            case "paint.colorCompressionTolerance": settings.Paint.ColorCompressionTolerance = value.GetDouble(); break;
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

    private static int RoundedInteger(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Number)
            throw new ArgumentException("Integer setting must be numeric.");
        if (value.TryGetInt32(out var exact))
            return exact;
        return checked((int)Math.Round(value.GetDouble(), MidpointRounding.AwayFromZero));
    }

    private static RegionMode ParseRegionMode(string? value)
    {
        if (Enum.TryParse<RegionMode>(value, true, out var mode) && Enum.IsDefined(mode))
            return mode;
        throw new ArgumentException("Region mode must be paint, fill, or skip.");
    }

    private void TryDeleteProgressSnapshot()
    {
        try
        {
            var path = Runtime.ProgressPath;
            if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // Best-effort cleanup only; stale progress is still rejected by timestamp.
        }
    }

    private ProgressSnapshot? ReadCurrentProgressSnapshot(bool liveOnly)
    {
        if (currentPaintStartedAt == DateTimeOffset.MinValue)
            return null;
        var preferredPath = Runtime.ProgressPath;
        var hasPreferredPath = !string.IsNullOrWhiteSpace(preferredPath);
        ProgressSnapshot? progress;
        DateTimeOffset writeTime;
        if (hasPreferredPath)
        {
            // The configured path belongs to the authenticated bridge instance.
            // Until its first atomic snapshot appears (or if it is malformed),
            // show no progress rather than borrowing another instance's file.
            progress = ReadProgressSnapshot(preferredPath, out writeTime);
            if (progress is null)
                return null;
        }
        else
        {
            progress = ReadFallbackProgressSnapshot(out writeTime);
        }
        if (progress is null)
            return null;
        var cutoff = currentPaintStartedAt.AddSeconds(-1);
        if (writeTime < cutoff)
        {
            if (hasPreferredPath)
                return null;
            progress = ReadFallbackProgressSnapshot(out writeTime);
            if (progress is null || writeTime < cutoff)
                return null;
        }
        if (liveOnly && !PaintRunning)
            return null;
        if (liveOnly && !currentProgressIsServerPaint)
            return null;
        return progress;
    }

    private ProgressSnapshot? ReadFallbackProgressSnapshot(out DateTimeOffset writeTime)
    {
        writeTime = DateTimeOffset.MinValue;
        if (currentPaintStartedAt == DateTimeOffset.MinValue)
            return null;
        var cutoff = currentPaintStartedAt.AddSeconds(-1);
        try
        {
            foreach (var path in ProgressSnapshotCandidatePaths(Paths, Runtime.ProgressPath))
            {
                var progress = ReadProgressSnapshot(path, out var candidateWriteTime);
                if (progress is null || candidateWriteTime < cutoff)
                    continue;
                writeTime = candidateWriteTime;
                return progress;
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
        }
        return null;
    }

    public static string[] ProgressSnapshotCandidatePaths(AppPaths paths, string? preferredPath = null)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var candidates = new List<string>();

        AddCandidate(preferredPath);
        AddProgressDirectory(paths.BridgeProgressDirectory);

        return candidates
            .OrderByDescending(SafeLastWriteTimeUtc)
            .ToArray();

        void AddProgressDirectory(string directory)
        {
            if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory))
                return;
            try
            {
                foreach (var path in Directory.EnumerateFiles(directory, "*.progress.json"))
                    AddCandidate(path);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
            }
        }

        void AddCandidate(string? path)
        {
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                return;
            try
            {
                var fullPath = Path.GetFullPath(path);
                if (seen.Add(fullPath))
                    candidates.Add(fullPath);
            }
            catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException or IOException or UnauthorizedAccessException)
            {
            }
        }

        static DateTime SafeLastWriteTimeUtc(string path)
        {
            try
            {
                return File.GetLastWriteTimeUtc(path);
            }
            catch
            {
                return DateTime.MinValue;
            }
        }
    }

    private void LogFinalProgressOnce()
    {
        if (!currentProgressIsServerPaint)
            return;
        if (finalProgressLogged)
            return;
        var progress = ReadCurrentProgressSnapshot(liveOnly: false);
        if (progress is null)
            return;
        LogReplayPassTransition(progress);
        var line = FormatProgressLogLine(progress);
        if (line.Length == 0)
            return;
        finalProgressLogged = true;
        Log.Info(line);
    }

    private void LogFailureProgressOnce()
    {
        if (!currentProgressIsServerPaint)
            return;
        if (finalProgressLogged)
            return;
        var progress = ReadCurrentProgressSnapshot(liveOnly: false);
        if (progress is null || !ShouldLogFailureProgress(progress))
            return;
        LogReplayPassTransition(progress);
        var line = FormatProgressLogLine(progress);
        if (line.Length == 0)
            return;
        finalProgressLogged = true;
        Log.Info(line);
    }

    private static bool ShouldLogFailureProgress(ProgressSnapshot progress)
    {
        var phase = progress.Phase.Trim().ToLowerInvariant();
        return phase is "failed" or "cancelled" ||
               phase.StartsWith("mesh_paint_", StringComparison.OrdinalIgnoreCase);
    }

    private void ResetReplayPassLog()
    {
        lock (replayPassLogGate)
            loggedReplayPasses.Clear();
    }

    private void LogReplayPassTransition(ProgressSnapshot? progress)
    {
        if (!currentProgressIsServerPaint || progress is null)
            return;
        var key = progress.ReplayCurrentPass.Trim().ToLowerInvariant();
        if (key is not ("fill" or "paint" or "complete"))
            return;
        lock (replayPassLogGate)
        {
            if (!loggedReplayPasses.Add(key))
                return;
        }
        var stage = ReplayProgressStageLabel(progress.ReplayProgressSource);
        var stageSuffix = stage.Length > 0 ? $" ({stage})" : "";
        Log.Info($"Paint: pass {ReplayPassLabel(key)}{stageSuffix}.");
    }

    private string FormatProgressLogLine(ProgressSnapshot progress)
    {
        var percent = progress.TotalSteps > 0
            ? Math.Clamp(progress.Step * 100.0 / progress.TotalSteps, 0.0, 100.0)
            : Math.Clamp(progress.Progress * 100.0, 0.0, 100.0);
        var rounded = (int)Math.Round(percent);
        var pass = FormatReplayPass(progress);
        return $"Paint: overall {rounded}% {ProgressBar(rounded)} | pass {pass} | pass ETA {FormatDuration(progress.ReplayCurrentPassEtaMs)} | total ETA {FormatEta(progress)} | elapsed {FormatDuration(progress.PaintElapsedMs)}";
    }

    private static string FormatReplayPass(ProgressSnapshot progress)
    {
        var stage = ReplayProgressStageLabel(progress.ReplayProgressSource);
        var pass = ReplayPassLabel(progress.ReplayCurrentPass);
        var passProgress = FormatReplayPassProgress(progress);
        return string.Join(' ', new[] { stage, pass, passProgress }.Where(value => !string.IsNullOrWhiteSpace(value) && value != "-")) switch
        {
            "" => "-",
            var detail => detail
        };
    }

    private static string ReplayPassLabel(string value) => value.Trim().ToLowerInvariant() switch
    {
        "fill" => "Fill",
        "paint" => "Paint",
        "complete" => "Complete",
        "" => "-",
        _ => value.Trim()
    };

    private static string ReplayProgressStageLabel(string value) => value.Trim().ToLowerInvariant() switch
    {
        "local_direct_submission" => "painting",
        "native_queue_backpressure" => "painting",
        "submission" => "queueing",
        _ => ""
    };

    private static string FormatReplayPassProgress(ProgressSnapshot progress)
    {
        var pass = ReplayPassLabel(progress.ReplayCurrentPass);
        var completed = progress.ReplayCurrentPassCompleted;
        var total = progress.ReplayCurrentPassTotal;
        if (total < 0 && progress.ReplayCurrentPassStart >= 0 && progress.ReplayCurrentPassEnd >= progress.ReplayCurrentPassStart)
        {
            total = progress.ReplayCurrentPassEnd - progress.ReplayCurrentPassStart;
            completed = Math.Clamp(progress.Step - progress.ReplayCurrentPassStart, 0, total);
        }
        if (pass == "Complete" && total <= 0 && progress.TotalSteps > 0)
        {
            completed = progress.TotalSteps;
            total = progress.TotalSteps;
        }
        if (total <= 0)
            return pass == "Complete" ? "100%" : "-";
        completed = Math.Clamp(completed, 0, total);
        var percent = (int)Math.Round(completed * 100.0 / total);
        return $"{completed}/{total} ({percent}%)";
    }

    private static string ProgressBar(int percent)
    {
        const int width = 16;
        var filled = Math.Clamp((int)Math.Round((percent / 100.0) * width), 0, width);
        return "[" + new string('#', filled) + new string('-', width - filled) + "]";
    }

    private static ProgressSnapshot? ReadProgressSnapshot(string path, out DateTimeOffset writeTime)
    {
        writeTime = DateTimeOffset.MinValue;
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            return null;
        try
        {
            writeTime = File.GetLastWriteTimeUtc(path);
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
                Number(root, "paint_elapsed_ms", Number(root, "elapsed_ms", -1.0)),
                Text(root, "replay_current_pass", ""),
                Int(root, "replay_current_pass_start", -1),
                Int(root, "replay_current_pass_end", -1),
                Text(root, "replay_progress_source", ""),
                Int(root, "replay_current_pass_completed", -1),
                Int(root, "replay_current_pass_total", -1),
                Number(root, "replay_current_pass_eta_ms", -1.0));
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
            return "0s";
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

    private static string FormatEta(ProgressSnapshot progress)
    {
        if (progress.Terminal)
            return string.Equals(progress.Result, "done", StringComparison.OrdinalIgnoreCase) ? "0s" : "-";

        return FormatDuration(progress.PaintEtaMs);
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
