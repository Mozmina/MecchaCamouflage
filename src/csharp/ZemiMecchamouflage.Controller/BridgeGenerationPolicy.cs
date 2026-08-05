namespace ZemiMecchamouflage.Controller;

public enum BridgeGenerationAction
{
    Reconnect,
    Replace,
    Inject,
    RestartRequired
}

public static class BridgeGenerationPolicy
{
    public static bool MayDeleteInstanceDirectory(
        bool ownerIsCurrentTarget,
        bool ownerProcessIsAlive,
        bool moduleIsLoaded) =>
        !moduleIsLoaded &&
        (ownerIsCurrentTarget || !ownerProcessIsAlive);

    public static BridgeGenerationAction Decide(
        bool hasResident,
        string? residentRuntimeBundleId,
        string desiredRuntimeBundleId,
        bool replacementAlreadyAttempted,
        int generationCount,
        int generationLimit)
    {
        _ = BridgeInstance.NormalizeHash(desiredRuntimeBundleId);
        if (generationCount < 0)
            throw new ArgumentOutOfRangeException(nameof(generationCount));
        if (generationLimit <= 0)
            throw new ArgumentOutOfRangeException(nameof(generationLimit));

        if (!hasResident)
            return generationCount >= generationLimit
                ? BridgeGenerationAction.RestartRequired
                : BridgeGenerationAction.Inject;
        if (residentRuntimeBundleId is not null &&
            string.Equals(residentRuntimeBundleId, desiredRuntimeBundleId, StringComparison.OrdinalIgnoreCase))
        {
            return BridgeGenerationAction.Reconnect;
        }
        if (replacementAlreadyAttempted || generationCount >= generationLimit)
            return BridgeGenerationAction.RestartRequired;
        return BridgeGenerationAction.Replace;
    }
}
