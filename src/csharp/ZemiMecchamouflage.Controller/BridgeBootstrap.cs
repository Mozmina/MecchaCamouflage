using System.Buffers.Binary;
using System.Text.Json;

namespace ZemiMecchamouflage.Controller;

/// <summary>
/// Legacy v1 bootstrap layout retained for authenticated v1.6.x migration and
/// compatibility tests. New injections always use <see cref="BridgeStartBlockV2"/>.
/// </summary>
public sealed class BridgeStartBlockV1
{
    public const uint Magic = 0x3153434D; // "MCS1" when written little-endian.
    public const uint AbiVersion = 1;
    public const int Size = 128;
    public const int GuidLength = 16;
    public const int TokenLength = 32;
    public const int HashLength = 32;

    private BridgeStartBlockV1(
        uint expectedPid,
        Guid instanceId,
        byte[] connectionToken,
        byte[] expectedBridgeHash,
        uint requestedPort,
        BridgeStartResultStateV1 resultState,
        uint boundPort,
        uint protocolVersion,
        uint win32Error,
        uint winsockError)
    {
        ExpectedPid = expectedPid;
        InstanceId = instanceId;
        ConnectionToken = connectionToken.ToArray();
        ExpectedBridgeHash = expectedBridgeHash.ToArray();
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
    public uint RequestedPort { get; }
    public BridgeStartResultStateV1 ResultState { get; }
    public uint BoundPort { get; }
    public uint ProtocolVersion { get; }
    public uint Win32Error { get; }
    public uint WinsockError { get; }

    public static BridgeStartBlockV1 Create(int expectedPid, Guid instanceId, ReadOnlySpan<byte> connectionToken, ReadOnlySpan<byte> expectedBridgeHash)
    {
        if (expectedPid <= 0)
            throw new ArgumentOutOfRangeException(nameof(expectedPid));
        if (connectionToken.Length != TokenLength)
            throw new ArgumentException($"A bridge token must be exactly {TokenLength} bytes.", nameof(connectionToken));
        if (!HasEntropy(connectionToken))
            throw new ArgumentException("A bridge token must not be all zeroes.", nameof(connectionToken));
        if (expectedBridgeHash.Length != HashLength)
            throw new ArgumentException($"A bridge hash must be exactly {HashLength} bytes.", nameof(expectedBridgeHash));

        return new BridgeStartBlockV1(
            (uint)expectedPid,
            instanceId,
            connectionToken.ToArray(),
            expectedBridgeHash.ToArray(),
            requestedPort: 0,
            BridgeStartResultStateV1.Uninitialized,
            boundPort: 0,
            BridgeProtocolV1.Version,
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
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(96, 4), RequestedPort);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(100, 4), (uint)ResultState);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(104, 4), BoundPort);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(108, 4), ProtocolVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(112, 4), Win32Error);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(116, 4), WinsockError);
        // Offsets 120..127 are reserved and deliberately remain zero.
        return bytes;
    }

    public static bool TryDeserialize(ReadOnlySpan<byte> bytes, out BridgeStartBlockV1 block, out string error)
    {
        block = null!;
        error = "";
        if (bytes.Length != Size)
        {
            error = $"Bridge start block must be exactly {Size} bytes.";
            return false;
        }
        if (BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(0, 4)) != Magic)
        {
            error = "Bridge start block has an invalid magic value.";
            return false;
        }
        if (BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(4, 4)) != Size ||
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(8, 4)) != AbiVersion)
        {
            error = "Bridge start block has an unsupported ABI version.";
            return false;
        }
        var requestedPort = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(96, 4));
        if (requestedPort != 0)
        {
            error = "Bridge start block must request an OS-selected port.";
            return false;
        }
        if (bytes.Slice(120, 8).ToArray().Any(value => value != 0))
        {
            error = "Bridge start block has non-zero reserved bytes.";
            return false;
        }
        if (!HasEntropy(bytes.Slice(32, TokenLength)))
        {
            error = "Bridge start block has an all-zero token.";
            return false;
        }

        block = new BridgeStartBlockV1(
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(12, 4)),
            new Guid(bytes.Slice(16, GuidLength), bigEndian: true),
            bytes.Slice(32, TokenLength).ToArray(),
            bytes.Slice(64, HashLength).ToArray(),
            requestedPort,
            (BridgeStartResultStateV1)BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(100, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(104, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(108, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(112, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(116, 4)));
        return true;
    }

    public static bool HasEntropy(ReadOnlySpan<byte> bytes)
    {
        foreach (var value in bytes)
        {
            if (value != 0)
                return true;
        }
        return false;
    }
}

public enum BridgeStartResultStateV1 : uint
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

public sealed record TargetProcessIdentity(int ProcessId, long CreationTimeUtcFileTime, string ExecutablePath)
{
    public static TargetProcessIdentity Create(int processId, long creationTimeUtcFileTime, string executablePath)
    {
        if (processId <= 0)
            throw new ArgumentOutOfRangeException(nameof(processId));
        if (creationTimeUtcFileTime <= 0)
            throw new ArgumentOutOfRangeException(nameof(creationTimeUtcFileTime));
        if (string.IsNullOrWhiteSpace(executablePath))
            throw new ArgumentException("The target executable path is required.", nameof(executablePath));
        return new TargetProcessIdentity(processId, creationTimeUtcFileTime, Path.GetFullPath(executablePath));
    }
}

/// <summary>Controller-owned identity for one direct bridge. Its token is never formatted or logged.</summary>
public sealed class BridgeInstance
{
    public BridgeInstance(
        TargetProcessIdentity target,
        Guid instanceId,
        ReadOnlySpan<byte> connectionToken,
        string expectedBridgeHash,
        string bridgePath,
        string injectorPath,
        string progressPath,
        string? expectedRuntimeBundleId = null,
        uint protocolVersion = BridgeProtocolV1.Version)
    {
        if (connectionToken.Length != BridgeStartBlockV1.TokenLength)
            throw new ArgumentException("A bridge token must be 32 bytes.", nameof(connectionToken));
        if (!BridgeStartBlockV1.HasEntropy(connectionToken))
            throw new ArgumentException("A bridge token must not be all zeroes.", nameof(connectionToken));
        Target = target;
        InstanceId = instanceId;
        ConnectionToken = connectionToken.ToArray();
        ExpectedBridgeHash = NormalizeHash(expectedBridgeHash);
        if (protocolVersion == BridgeProtocolV2.Version)
        {
            if (string.IsNullOrWhiteSpace(expectedRuntimeBundleId))
                throw new ArgumentException("A V2 bridge instance requires a runtime bundle identity.", nameof(expectedRuntimeBundleId));
            ExpectedRuntimeBundleId = NormalizeHash(expectedRuntimeBundleId);
        }
        else if (protocolVersion != BridgeProtocolV1.Version)
        {
            throw new ArgumentOutOfRangeException(nameof(protocolVersion), "The bridge protocol version is unsupported.");
        }
        ProtocolVersion = protocolVersion;
        BridgePath = bridgePath;
        InjectorPath = injectorPath;
        ProgressPath = progressPath;
    }

    public TargetProcessIdentity Target { get; }
    public Guid InstanceId { get; }
    public byte[] ConnectionToken { get; }
    public string ExpectedBridgeHash { get; }
    public string? ExpectedRuntimeBundleId { get; }
    public uint ProtocolVersion { get; }
    public string BridgePath { get; }
    public string InjectorPath { get; }
    public string ProgressPath { get; private set; }
    public int? Port { get; private set; }

    public void SetPort(int port)
    {
        if (port is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(port));
        Port = port;
    }

    public bool TrySetProgressPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) ||
            path.IndexOf('\0', StringComparison.Ordinal) >= 0 ||
            !Path.IsPathFullyQualified(path))
        {
            return false;
        }
        ProgressPath = Path.GetFullPath(path);
        return true;
    }

    public BridgeEndpoint Endpoint => Port is int port
        ? new BridgeEndpoint(
            "127.0.0.1",
            port,
            InstanceId,
            ConnectionToken,
            ExpectedBridgeHash,
            ProtocolVersion,
            ExpectedRuntimeBundleId)
        : throw new InvalidOperationException("The direct bridge has not reported a port.");

    public override string ToString() => $"BridgeInstance {{ TargetPid = {Target.ProcessId}, InstanceId = {InstanceId:N}, Port = {Port?.ToString() ?? "(not started)"}, BridgePath = {BridgePath} }}";

    internal static string NormalizeHash(string value)
    {
        var normalized = value?.Trim().ToLowerInvariant() ?? "";
        if (normalized.Length != BridgeStartBlockV1.HashLength * 2 || !normalized.All(Uri.IsHexDigit))
            throw new ArgumentException("A bridge hash must be a 64-character SHA-256 hex string.", nameof(value));
        return normalized;
    }
}

public sealed class BridgeEndpoint
{
    public BridgeEndpoint(
        string host,
        int port,
        Guid instanceId,
        ReadOnlySpan<byte> connectionToken,
        string expectedBridgeHash,
        uint protocolVersion,
        string? expectedRuntimeBundleId = null)
    {
        if (string.IsNullOrWhiteSpace(host))
            throw new ArgumentException("A bridge host is required.", nameof(host));
        if (port is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(port));
        if (connectionToken.Length != BridgeStartBlockV1.TokenLength)
            throw new ArgumentException("A bridge token must be 32 bytes.", nameof(connectionToken));
        if (!BridgeStartBlockV1.HasEntropy(connectionToken))
            throw new ArgumentException("A bridge token must not be all zeroes.", nameof(connectionToken));
        Host = host;
        Port = port;
        InstanceId = instanceId;
        ConnectionToken = connectionToken.ToArray();
        ExpectedBridgeHash = BridgeInstance.NormalizeHash(expectedBridgeHash);
        ProtocolVersion = protocolVersion;
        if (protocolVersion == BridgeProtocolV2.Version)
        {
            if (string.IsNullOrWhiteSpace(expectedRuntimeBundleId))
                throw new ArgumentException("A V2 bridge endpoint requires a runtime bundle identity.", nameof(expectedRuntimeBundleId));
            ExpectedRuntimeBundleId = BridgeInstance.NormalizeHash(expectedRuntimeBundleId);
        }
        else if (protocolVersion != BridgeProtocolV1.Version)
        {
            throw new ArgumentOutOfRangeException(nameof(protocolVersion), "The bridge protocol version is unsupported.");
        }
    }

    public string Host { get; }
    public int Port { get; }
    public Guid InstanceId { get; }
    public byte[] ConnectionToken { get; }
    public string ExpectedBridgeHash { get; }
    public string? ExpectedRuntimeBundleId { get; }
    public uint ProtocolVersion { get; }

    public override string ToString() => $"BridgeEndpoint {{ Host = {Host}, Port = {Port}, InstanceId = {InstanceId:N} }}";
}

public sealed record BridgeHelloIdentity(
    int ProcessId,
    Guid InstanceId,
    string BridgeHash,
    uint ProtocolVersion,
    string? ProgressPath,
    string? RuntimeBundleId);

public static class BridgeProtocolV1
{
    public const uint Version = 1;

    public static string CreateHello(BridgeEndpoint endpoint) => JsonSerializer.Serialize(new
    {
        type = "hello",
        bootstrap_protocol = endpoint.ProtocolVersion,
        instance_id = endpoint.InstanceId.ToString("N"),
        token = Convert.ToHexString(endpoint.ConnectionToken).ToLowerInvariant()
    });

    public static bool TryValidateHelloReply(string raw, BridgeEndpoint endpoint, int expectedPid, out BridgeHelloIdentity identity, out string error)
    {
        identity = null!;
        error = "";
        try
        {
            using var document = JsonDocument.Parse(raw);
            var root = document.RootElement;
            if (!root.TryGetProperty("success", out var success) || !success.GetBoolean() ||
                !root.TryGetProperty("stage", out var stage) || !string.Equals(stage.GetString(), "hello", StringComparison.Ordinal) ||
                !root.TryGetProperty("metadata", out var metadata) || metadata.ValueKind != JsonValueKind.Object)
            {
                error = "Bridge rejected the bootstrap hello.";
                return false;
            }
            if (!metadata.TryGetProperty("pid", out var pidElement) || !pidElement.TryGetInt32(out var pid) || pid != expectedPid ||
                !metadata.TryGetProperty("instance_id", out var instanceElement) || !Guid.TryParseExact(instanceElement.GetString(), "N", out var instanceId) || instanceId != endpoint.InstanceId ||
                !metadata.TryGetProperty("bridge_hash", out var hashElement) || !string.Equals(hashElement.GetString(), endpoint.ExpectedBridgeHash, StringComparison.OrdinalIgnoreCase) ||
                !metadata.TryGetProperty("protocol_version", out var versionElement) || !versionElement.TryGetUInt32(out var version) || version != endpoint.ProtocolVersion)
            {
                error = "Bridge hello identity did not match the requested instance.";
                return false;
            }
            string? runtimeBundleId = null;
            if (endpoint.ProtocolVersion == BridgeProtocolV2.Version)
            {
                if (!metadata.TryGetProperty("runtime_bundle_id", out var bundleElement) ||
                    bundleElement.ValueKind != JsonValueKind.String ||
                    !string.Equals(bundleElement.GetString(), endpoint.ExpectedRuntimeBundleId, StringComparison.OrdinalIgnoreCase))
                {
                    error = "Bridge hello runtime bundle identity did not match the requested generation.";
                    return false;
                }
                runtimeBundleId = endpoint.ExpectedRuntimeBundleId;
            }
            string? progressPath = null;
            if (metadata.TryGetProperty("progress_path", out var progressPathElement) &&
                progressPathElement.ValueKind == JsonValueKind.String)
            {
                var reportedPath = progressPathElement.GetString();
                if (!string.IsNullOrWhiteSpace(reportedPath) &&
                    reportedPath.IndexOf('\0', StringComparison.Ordinal) < 0 &&
                    Path.IsPathFullyQualified(reportedPath))
                {
                    progressPath = Path.GetFullPath(reportedPath);
                }
            }
            identity = new BridgeHelloIdentity(
                pid,
                instanceId,
                endpoint.ExpectedBridgeHash,
                version,
                progressPath,
                runtimeBundleId);
            return true;
        }
        catch (JsonException ex)
        {
            error = "Bridge hello response was invalid JSON: " + ex.Message;
            return false;
        }
    }
}

public sealed record InjectorResult(
    bool Success,
    string State,
    uint ProtocolVersion,
    int? ProcessId,
    Guid? InstanceId,
    string? BridgeHash,
    string? RuntimeBundleId,
    int? BoundPort,
    int Win32Error,
    int WinsockError,
    string Detail)
{
    public bool IsListening => Success && string.Equals(State, "listening", StringComparison.OrdinalIgnoreCase);

    public bool Matches(
        int expectedPid,
        Guid expectedInstanceId,
        string expectedBridgeHash,
        string? expectedRuntimeBundleId = null) =>
        IsListening &&
        ProtocolVersion == (expectedRuntimeBundleId is null ? BridgeProtocolV1.Version : BridgeProtocolV2.Version) &&
        ProcessId == expectedPid &&
        InstanceId == expectedInstanceId &&
        string.Equals(BridgeHash, expectedBridgeHash, StringComparison.OrdinalIgnoreCase) &&
        string.Equals(RuntimeBundleId, expectedRuntimeBundleId, StringComparison.OrdinalIgnoreCase) &&
        BoundPort is >= 1 and <= 65535;

    public static bool TryParseFinal(string stdout, out InjectorResult result, out string error)
    {
        result = null!;
        error = "";
        foreach (var line in stdout.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries).Reverse())
        {
            try
            {
                using var document = JsonDocument.Parse(line);
                var root = document.RootElement;
                if (!root.TryGetProperty("event", out var eventElement) || !string.Equals(eventElement.GetString(), "result", StringComparison.Ordinal))
                    continue;
                if (!root.TryGetProperty("protocol", out var protocolElement) || !protocolElement.TryGetUInt32(out var protocol) ||
                    !root.TryGetProperty("success", out var successElement) ||
                    !root.TryGetProperty("state", out var stateElement))
                {
                    error = "Injector final result omitted required fields.";
                    return false;
                }
                var processId = TryGetInt32(root, "pid");
                var instanceText = TryGetString(root, "instance_id");
                var instanceId = Guid.TryParseExact(instanceText, "N", out var parsedInstance) ? parsedInstance : (Guid?)null;
                var hash = TryGetString(root, "bridge_hash");
                result = new InjectorResult(
                    successElement.GetBoolean(),
                    stateElement.GetString() ?? "",
                    protocol,
                    processId,
                    instanceId,
                    hash,
                    TryGetString(root, "runtime_bundle_id"),
                    TryGetInt32(root, "port"),
                    TryGetInt32(root, "win32") ?? 0,
                    TryGetInt32(root, "winsock") ?? 0,
                    TryGetString(root, "detail") ?? "");
                return true;
            }
            catch (JsonException)
            {
                // Diagnostics may contain non-JSON stderr-like lines. Only a final result is authoritative.
            }
        }
        error = "Injector did not emit a final result record.";
        return false;
    }

    private static int? TryGetInt32(JsonElement root, string property) =>
        root.TryGetProperty(property, out var element) && element.TryGetInt32(out var value) ? value : null;

    private static string? TryGetString(JsonElement root, string property) =>
        root.TryGetProperty(property, out var element) && element.ValueKind == JsonValueKind.String ? element.GetString() : null;
}

public static class BridgeInstanceNaming
{
    public static string CreateDirectoryName(string runtimeBundleId, Guid instanceId, int processId)
    {
        if (processId <= 0)
            throw new ArgumentOutOfRangeException(nameof(processId));
        var normalized = BridgeInstance.NormalizeHash(runtimeBundleId);
        return $"bridge-instance-v2-{processId}-{normalized[..16]}-{instanceId:N}";
    }

    public static string CreateBridgeFileName(string runtimeBundleId, Guid instanceId)
    {
        var normalizedBundle = BridgeInstance.NormalizeHash(runtimeBundleId);
        return $"meccha-direct-bridge-v2-{normalizedBundle[..16]}-{instanceId:N}.dll";
    }
}
