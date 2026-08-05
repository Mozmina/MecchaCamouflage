using System.Buffers.Binary;

namespace ZemiMecchamouflage.Controller;

public sealed record BridgeResidentCoreIdentity(
    int ProcessId,
    int Port,
    uint ProtocolVersion,
    Guid InstanceId,
    byte[] ConnectionToken,
    string BridgeHash,
    string? RuntimeBundleId)
{
    public bool IsLegacy => ProtocolVersion == BridgeProtocolV1.Version && RuntimeBundleId is null;
}

public static class BridgeResidentCore
{
    public const uint MagicV1 = 0x3152434D;
    public const uint MagicV2 = 0x3252434D;
    public const int SizeV1 = 104;
    public const int SizeV2 = 136;

    public static bool TryParse(ReadOnlySpan<byte> bytes, int expectedProcessId, out BridgeResidentCoreIdentity identity)
    {
        identity = null!;
        if (expectedProcessId <= 0 || bytes.Length < 12)
            return false;

        var magic = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(0, 4));
        var size = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(4, 4));
        var abi = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(8, 4));
        var legacy = magic == MagicV1 && size == SizeV1 && abi == 1;
        var current = magic == MagicV2 && size == SizeV2 && abi == NativeRuntimeBundle.ResidentCoreAbi;
        if ((!legacy && !current) || bytes.Length != size)
            return false;

        var pid = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(12, 4));
        var port = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(16, 4));
        var protocol = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(20, 4));
        if (pid != expectedProcessId ||
            port is 0 or > 65535 ||
            protocol != (legacy ? BridgeProtocolV1.Version : BridgeProtocolV2.Version) ||
            !BridgeStartBlockV1.HasEntropy(bytes.Slice(40, BridgeStartBlockV1.TokenLength)) ||
            !BridgeStartBlockV1.HasEntropy(bytes.Slice(72, BridgeStartBlockV1.HashLength)))
        {
            return false;
        }

        string? runtimeBundleId = null;
        if (current)
        {
            var bundle = bytes.Slice(104, BridgeStartBlockV2.HashLength);
            if (!BridgeStartBlockV1.HasEntropy(bundle))
                return false;
            runtimeBundleId = Convert.ToHexString(bundle).ToLowerInvariant();
        }

        identity = new BridgeResidentCoreIdentity(
            (int)pid,
            (int)port,
            protocol,
            new Guid(bytes.Slice(24, BridgeStartBlockV1.GuidLength), bigEndian: true),
            bytes.Slice(40, BridgeStartBlockV1.TokenLength).ToArray(),
            Convert.ToHexString(bytes.Slice(72, BridgeStartBlockV1.HashLength)).ToLowerInvariant(),
            runtimeBundleId);
        return true;
    }
}
