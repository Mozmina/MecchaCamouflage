namespace ZemiMecchamouflage.Controller;

public sealed record NativePresentStatusLogSample
{
    public string Status { get; init; } = "unknown";
    public string Reason { get; init; } = "";
    public string Format { get; init; } = "";
    public string ConfiguredScope { get; init; } = "unknown";
    public string CaptureStatus { get; init; } = "unknown";
    public ulong CaptureAgeMs { get; init; }
    public ulong HudRebinds { get; init; }
    public uint HiderRosterCount { get; init; }
    public uint HunterRosterCount { get; init; }
    public string RosterSource { get; init; } = "unknown";
    public uint RosterCount { get; init; }
    public uint ValidPawns { get; init; }
    public uint FilteredLocal { get; init; }
    public uint FilteredSpectators { get; init; }
    public uint FilteredScope { get; init; }
    public uint MissingCapsuleContracts { get; init; }
    public uint MissingMeshContracts { get; init; }
    public uint MissingPoseContracts { get; init; }
    public uint TargetsWithoutSpatialGeometry { get; init; }
    public uint TargetFaults { get; init; }
    public uint CapsuleComponents { get; init; }
    public uint CapsuleTransforms { get; init; }
    public uint CapsuleSizes { get; init; }
    public uint CapsuleProjected { get; init; }
    public uint MeshComponents { get; init; }
    public uint MeshTransforms { get; init; }
    public ulong SkeletonContracts { get; init; }
    public uint PoseProfileMatches { get; init; }
    public uint PoseComponentSpace { get; init; }
    public uint PoseBones { get; init; }
    public uint PoseEdges { get; init; }
    public uint Poses { get; init; }
    public uint Players { get; init; }
    public uint Lines { get; init; }
    public uint Texts { get; init; }
    public ulong Vertices { get; init; }
    public ulong GlyphQuads { get; init; }
    public double ProjectionScaleX { get; init; } = 1.0;
    public double ProjectionScaleY { get; init; } = 1.0;
    public ulong ProjectionCalibrations { get; init; }
    public ulong SnapshotSequence { get; init; }
    public ulong InitializationAgeMs { get; init; }
    public string InitializationStage { get; init; } = "unknown";
    public bool PresentHookReady { get; init; }
    public bool ExecuteHookReady { get; init; }
    public bool ResizeHookReady { get; init; }
    public bool FactoryHooksReady { get; init; }
    public bool GameSwapchainSeen { get; init; }
    public ulong PresentCalls { get; init; }
    public ulong ExecuteCalls { get; init; }
    public uint SwapchainQueueRecords { get; init; }
    public uint RecentDirectQueueRecords { get; init; }
    public ulong SubmittedFrames { get; init; }
    public ulong CompletedFences { get; init; }
    public ulong RenderedFrames { get; init; }
}

public static class NativePresentStatusLogPolicy
{
    private const ulong InitializationWarningAgeMs = 10_000;

    public static string Format(NativePresentStatusLogSample sample, bool verbose)
    {
        if (verbose || ShouldWarn(sample))
            return FormatVerbose(sample);

        var expectedPlayers = ExpectedTargets(sample);
        return
            $"ESP: status={sample.Status} | scope={sample.ConfiguredScope} | " +
            $"capture={sample.CaptureStatus} | " +
            $"roster={sample.RosterSource}:{sample.RosterCount} | " +
            $"drawable={sample.Players}/{expectedPlayers} | " +
            $"filtered=local:{sample.FilteredLocal} " +
            $"spectator:{sample.FilteredSpectators} scope:{sample.FilteredScope} | " +
            $"miss=capsule:{sample.MissingCapsuleContracts} " +
            $"mesh:{sample.MissingMeshContracts} " +
            $"pose:{sample.MissingPoseContracts} " +
            $"spatial:{sample.TargetsWithoutSpatialGeometry} " +
            $"fault:{sample.TargetFaults}";
    }

    public static string Signature(NativePresentStatusLogSample sample, bool verbose)
    {
        var expectedTargets = ExpectedTargets(sample);
        var stable = string.Join('|',
            sample.Status,
            ReasonState(sample),
            sample.Format,
            sample.ConfiguredScope,
            sample.CaptureStatus,
            sample.HudRebinds,
            Presence((ulong)sample.HiderRosterCount + sample.HunterRosterCount),
            sample.RosterSource,
            Presence(sample.RosterCount),
            Coverage(sample.ValidPawns, sample.RosterCount),
            Presence(sample.MissingCapsuleContracts),
            Presence(sample.MissingMeshContracts),
            Presence(sample.MissingPoseContracts),
            Presence(sample.TargetsWithoutSpatialGeometry),
            Presence(sample.TargetFaults),
            Coverage(sample.CapsuleComponents, expectedTargets),
            Coverage(sample.CapsuleTransforms, sample.CapsuleComponents),
            Coverage(sample.CapsuleSizes, sample.CapsuleTransforms),
            Coverage(sample.MeshComponents, expectedTargets),
            Coverage(sample.MeshTransforms, sample.MeshComponents),
            Presence(sample.SkeletonContracts),
            Coverage(sample.PoseProfileMatches, expectedTargets),
            Coverage(sample.PoseComponentSpace, sample.PoseProfileMatches),
            Presence(sample.PoseBones),
            Coverage(sample.Poses, sample.PoseProfileMatches),
            ProjectionState(sample),
            Presence(sample.SnapshotSequence),
            InitializationState(sample),
            sample.InitializationStage,
            sample.PresentHookReady,
            sample.ExecuteHookReady,
            sample.ResizeHookReady,
            sample.FactoryHooksReady,
            sample.GameSwapchainSeen,
            Presence(sample.PresentCalls),
            Presence(sample.ExecuteCalls),
            Presence(sample.SwapchainQueueRecords),
            Presence(sample.RecentDirectQueueRecords),
            Presence(sample.SubmittedFrames),
            Presence(sample.CompletedFences),
            Presence(sample.RenderedFrames));

        if (!verbose)
            return stable;

        return string.Join('|',
            stable,
            sample.CaptureAgeMs,
            sample.RosterCount,
            sample.ValidPawns,
            sample.CapsuleComponents,
            sample.CapsuleTransforms,
            sample.CapsuleSizes,
            sample.CapsuleProjected,
            sample.MissingCapsuleContracts,
            sample.MissingMeshContracts,
            sample.MissingPoseContracts,
            sample.TargetsWithoutSpatialGeometry,
            sample.TargetFaults,
            sample.MeshComponents,
            sample.MeshTransforms,
            sample.SkeletonContracts,
            sample.PoseProfileMatches,
            sample.PoseComponentSpace,
            sample.PoseBones,
            sample.PoseEdges,
            sample.Poses,
            sample.Players,
            sample.Lines,
            sample.Texts,
            sample.Vertices,
            sample.GlyphQuads,
            Math.Round(sample.ProjectionScaleX, 4),
            Math.Round(sample.ProjectionScaleY, 4),
            sample.ProjectionCalibrations,
            sample.SnapshotSequence,
            sample.InitializationAgeMs,
            sample.PresentCalls,
            sample.ExecuteCalls,
            sample.SwapchainQueueRecords,
            sample.RecentDirectQueueRecords,
            sample.SubmittedFrames,
            sample.CompletedFences,
            sample.RenderedFrames);
    }

    public static bool ShouldWarn(NativePresentStatusLogSample sample) =>
        string.Equals(sample.Status, "unavailable", StringComparison.OrdinalIgnoreCase) ||
        IsInitializationStalled(sample) ||
        string.Equals(sample.CaptureStatus, "stalled", StringComparison.OrdinalIgnoreCase) ||
        sample.MissingCapsuleContracts > 0 ||
        sample.MissingMeshContracts > 0 ||
        sample.MissingPoseContracts > 0 ||
        sample.TargetsWithoutSpatialGeometry > 0 ||
        sample.TargetFaults > 0;

    private static string FormatVerbose(NativePresentStatusLogSample sample) =>
        $"ESP: native Present status={sample.Status}; scope={sample.ConfiguredScope}; " +
        $"format={sample.Format}; snapshot_sequence={sample.SnapshotSequence}; " +
        $"init={sample.InitializationStage} age_ms={sample.InitializationAgeMs}; " +
        $"hooks=present:{sample.PresentHookReady} execute:{sample.ExecuteHookReady} " +
        $"resize:{sample.ResizeHookReady} factory:{sample.FactoryHooksReady}; " +
        $"traffic=present:{sample.PresentCalls} execute:{sample.ExecuteCalls}; " +
        $"association=swapchain:{sample.GameSwapchainSeen} " +
        $"exact:{sample.SwapchainQueueRecords} recent:{sample.RecentDirectQueueRecords}; " +
        $"capture={sample.CaptureStatus} age_ms={sample.CaptureAgeMs}; " +
        $"hud_rebinds={sample.HudRebinds}; " +
        $"roles={sample.HiderRosterCount}/{sample.HunterRosterCount}; " +
        $"roster={sample.RosterSource}:{sample.RosterCount}; " +
        $"pawns={sample.ValidPawns}; " +
        $"filtered={sample.FilteredLocal}/{sample.FilteredSpectators}/{sample.FilteredScope}; " +
        $"missing=capsule:{sample.MissingCapsuleContracts} " +
        $"mesh:{sample.MissingMeshContracts} " +
        $"pose:{sample.MissingPoseContracts} " +
        $"spatial:{sample.TargetsWithoutSpatialGeometry} " +
        $"fault:{sample.TargetFaults}; " +
        $"capsule={sample.CapsuleComponents}/{sample.CapsuleTransforms}/" +
        $"{sample.CapsuleSizes}/{sample.CapsuleProjected}; " +
        $"mesh={sample.MeshComponents}/{sample.MeshTransforms}; " +
        $"pose=contracts:{sample.SkeletonContracts} profiles:{sample.PoseProfileMatches} " +
        $"component_space:{sample.PoseComponentSpace} " +
        $"bones:{sample.PoseBones} edges:{sample.PoseEdges} ready:{sample.Poses}; " +
        $"geometry={sample.Players}/{sample.Lines}/{sample.Texts}; " +
        $"vertices={sample.Vertices}; glyph_quads={sample.GlyphQuads}; " +
        $"projection_scale={sample.ProjectionScaleX:F4}/{sample.ProjectionScaleY:F4} " +
        $"calibrations={sample.ProjectionCalibrations}; " +
        $"submitted_frames={sample.SubmittedFrames}; " +
        $"completed_fences={sample.CompletedFences}; " +
        $"rendered_frames={sample.RenderedFrames}; reason={sample.Reason}.";

    private static string Presence(ulong value) => value == 0 ? "none" : "present";

    private static ulong ExpectedTargets(NativePresentStatusLogSample sample)
    {
        var filtered =
            (ulong)sample.FilteredLocal +
            sample.FilteredSpectators +
            sample.FilteredScope;
        return sample.ValidPawns > filtered
            ? (ulong)sample.ValidPawns - filtered
            : 0;
    }

    private static string ReasonState(NativePresentStatusLogSample sample) =>
        string.Equals(sample.Status, "ready", StringComparison.OrdinalIgnoreCase) &&
        string.Equals(sample.CaptureStatus, "active", StringComparison.OrdinalIgnoreCase)
            ? "healthy"
            : sample.Reason;

    private static string InitializationState(NativePresentStatusLogSample sample)
    {
        if (!string.Equals(
                sample.Status,
                "initializing",
                StringComparison.OrdinalIgnoreCase))
        {
            return "inactive";
        }
        return IsInitializationStalled(sample) ? "stalled" : "starting";
    }

    private static bool IsInitializationStalled(
        NativePresentStatusLogSample sample) =>
        string.Equals(
            sample.Status,
            "initializing",
            StringComparison.OrdinalIgnoreCase) &&
        sample.InitializationAgeMs >= InitializationWarningAgeMs;

    private static string Coverage(ulong value, ulong expected)
    {
        if (expected == 0)
            return value == 0 ? "idle" : "unexpected";
        if (value == 0)
            return "none";
        return value < expected ? "partial" : "complete";
    }

    private static string ProjectionState(NativePresentStatusLogSample sample)
    {
        if (sample.ProjectionCalibrations == 0)
            return "uncalibrated";
        return double.IsFinite(sample.ProjectionScaleX) &&
               double.IsFinite(sample.ProjectionScaleY) &&
               sample.ProjectionScaleX > 0 &&
               sample.ProjectionScaleY > 0
            ? "calibrated"
            : "invalid";
    }
}
