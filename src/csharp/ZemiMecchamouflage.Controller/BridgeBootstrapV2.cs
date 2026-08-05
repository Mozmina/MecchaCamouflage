using System.Buffers.Binary;

namespace ZemiMecchamouflage.Controller;

public enum BridgeStartResultStateV2 : uint
{
    Uninitialized = 0,
    Starting = 1,
    Listening = 2,
    InvalidBlock = 3,
    ProcessMismatch = 4,
    AlreadyStarted = 5,
    WinsockFailed = 6,
    SocketFailed = 7,
    BindFailed = 8,
    ListenFailed = 9,
    WorkerFailed = 10
}

public sealed class BridgeStartBlockV2
{
    public const uint Magic = 0x3253434D; // "MCS2" when written little-endian.
    public const uint AbiVersion = NativeRuntimeBundle.StartBlockAbi;
    public const int Size = 160;
    public const int GuidLength = 16;
    public const int TokenLength = 32;
    public const int HashLength = 32;

    private BridgeStartBlockV2(
        uint expectedPid,
        Guid instanceId,
        byte[] connectionToken,
        byte[] expectedBridgeHash,
        byte[] expectedRuntimeBundleHash,
        uint requestedPort,
        BridgeStartResultStateV2 resultState,
        uint boundPort,
        uint protocolVersion,
        uint win32Error,
        uint winsockError)
    {
        ExpectedPid = expectedPid;
        InstanceId = instanceId;
        ConnectionToken = connectionToken.ToArray();
        ExpectedBridgeHash = expectedBridgeHash.ToArray();
        ExpectedRuntimeBundleHash = expectedRuntimeBundleHash.ToArray();
        RequestedPort = requestedPort;
        ResultState = resultState;
        BoundPort = boundPort;
        ProtocolVersion = protocolVersion;
        Win32Error = win32Error;
        WinsockError = winsockError;
    }

    public uint ExpectedPid { get; }
    public Guid InstanceId { get; }
    public byte[] ConnectionToken { get; }
    public byte[] ExpectedBridgeHash { get; }
    public byte[] ExpectedRuntimeBundleHash { get; }
    public uint RequestedPort { get; }
    public BridgeStartResultStateV2 ResultState { get; }
    public uint BoundPort { get; }
    public uint ProtocolVersion { get; }
    public uint Win32Error { get; }
    public uint WinsockError { get; }

    public static BridgeStartBlockV2 Create(
        int expectedPid,
        Guid instanceId,
        ReadOnlySpan<byte> connectionToken,
        ReadOnlySpan<byte> expectedBridgeHash,
        ReadOnlySpan<byte> expectedRuntimeBundleHash)
    {
        if (expectedPid <= 0)
            throw new ArgumentOutOfRangeException(nameof(expectedPid));
        ValidateBytes(connectionToken, TokenLength, "bridge token", nameof(connectionToken));
        ValidateBytes(expectedBridgeHash, HashLength, "bridge hash", nameof(expectedBridgeHash));
        ValidateBytes(expectedRuntimeBundleHash, HashLength, "runtime bundle hash", nameof(expectedRuntimeBundleHash));

        return new BridgeStartBlockV2(
            (uint)expectedPid,
            instanceId,
            connectionToken.ToArray(),
            expectedBridgeHash.ToArray(),
            expectedRuntimeBundleHash.ToArray(),
            requestedPort: 0,
            BridgeStartResultStateV2.Uninitialized,
            boundPort: 0,
            BridgeProtocolV2.Version,
            win32Error: 0,
            winsockError: 0);
    }

    public byte[] Serialize()
    {
        var bytes = new byte[Size];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(0, 4), Magic);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4, 4), Size);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8, 4), AbiVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12, 4), ExpectedPid);
        if (!InstanceId.TryWriteBytes(bytes.AsSpan(16, GuidLength), bigEndian: true, out var written) || written != GuidLength)
            throw new InvalidOperationException("Could not serialize the bridge instance GUID.");
        ConnectionToken.CopyTo(bytes, 32);
        ExpectedBridgeHash.CopyTo(bytes, 64);
        ExpectedRuntimeBundleHash.CopyTo(bytes, 96);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(128, 4), RequestedPort);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(132, 4), (uint)ResultState);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(136, 4), BoundPort);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(140, 4), ProtocolVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(144, 4), Win32Error);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(148, 4), WinsockError);
        return bytes;
    }

    public static bool TryDeserialize(ReadOnlySpan<byte> bytes, out BridgeStartBlockV2 block, out string error)
    {
        block = null!;
        error = "";
        if (bytes.Length != Size)
        {
            error = $"Bridge V2 start block must be exactly {Size} bytes.";
            return false;
        }
        if (BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(0, 4)) != Magic ||
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(4, 4)) != Size ||
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(8, 4)) != AbiVersion)
        {
            error = "Bridge V2 start block has an unsupported ABI.";
            return false;
        }
        if (BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(128, 4)) != 0 ||
            bytes.Slice(152, 8).IndexOfAnyExcept((byte)0) >= 0)
        {
            error = "Bridge V2 start block contains unsupported input fields.";
            return false;
        }
        if (!BridgeStartBlockV1.HasEntropy(bytes.Slice(32, TokenLength)) ||
            !BridgeStartBlockV1.HasEntropy(bytes.Slice(64, HashLength)) ||
            !BridgeStartBlockV1.HasEntropy(bytes.Slice(96, HashLength)))
        {
            error = "Bridge V2 start block contains an all-zero identity field.";
            return false;
        }

        block = new BridgeStartBlockV2(
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(12, 4)),
            new Guid(bytes.Slice(16, GuidLength), bigEndian: true),
            bytes.Slice(32, TokenLength).ToArray(),
            bytes.Slice(64, HashLength).ToArray(),
            bytes.Slice(96, HashLength).ToArray(),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(128, 4)),
            (BridgeStartResultStateV2)BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(132, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(136, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(140, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(144, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(148, 4)));
        return true;
    }

    private static void ValidateBytes(ReadOnlySpan<byte> bytes, int length, string label, string parameter)
    {
        if (bytes.Length != length)
            throw new ArgumentException($"A {label} must be exactly {length} bytes.", parameter);
        if (!BridgeStartBlockV1.HasEntropy(bytes))
            throw new ArgumentException($"A {label} must not be all zeroes.", parameter);
    }
}

public static class BridgeProtocolV2
{
    public const uint Version = NativeRuntimeBundle.ProtocolVersion;
}
