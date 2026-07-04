using System.Net.Sockets;
using System.Text;

namespace MecchaCamouflage.Wpf;

public sealed record BridgeReply(bool Ok, bool Success, string Stage, string Message, string Raw);

public sealed class BridgeClient
{
    private readonly string host;
    private readonly int port;
    private readonly TimeSpan timeout;

    public BridgeClient(string host = "127.0.0.1", int port = 50262, TimeSpan? timeout = null)
    {
        this.host = host;
        this.port = port;
        this.timeout = timeout ?? TimeSpan.FromSeconds(5);
    }

    public async Task<BridgeReply> RequestAsync(string jsonLine, CancellationToken cancellationToken = default)
    {
        try
        {
            using var client = new TcpClient();
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(timeout);
            await client.ConnectAsync(host, port, cts.Token);
            await using var stream = client.GetStream();
            var request = Encoding.UTF8.GetBytes(jsonLine.EndsWith('\n') ? jsonLine : jsonLine + "\n");
            await stream.WriteAsync(request, cts.Token);
            await stream.FlushAsync(cts.Token);
            using var reader = new StreamReader(stream, Encoding.UTF8, leaveOpen: true);
            var response = await reader.ReadToEndAsync(cts.Token);
            return Parse(response);
        }
        catch (Exception ex)
        {
            return new BridgeReply(false, false, "transport_error", ex.Message, "");
        }
    }

    public Task<BridgeReply> PingAsync(CancellationToken cancellationToken = default) =>
        RequestAsync("{\"type\":\"ping\"}", cancellationToken);

    public Task<BridgeReply> CancelPaintAsync(CancellationToken cancellationToken = default) =>
        RequestAsync("{\"type\":\"cancel_paint\"}", cancellationToken);

    public Task<BridgeReply> ShutdownAsync(CancellationToken cancellationToken = default) =>
        RequestAsync("{\"type\":\"shutdown\"}", cancellationToken);

    private static BridgeReply Parse(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw))
            return new BridgeReply(false, false, "empty_response", "Bridge returned no response.", raw);
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(raw);
            var root = doc.RootElement;
            var success = root.TryGetProperty("success", out var successProp) && successProp.GetBoolean();
            var stage = root.TryGetProperty("stage", out var stageProp) ? stageProp.GetString() ?? "" : "";
            var message = root.TryGetProperty("message", out var messageProp) ? messageProp.GetString() ?? "" : "";
            return new BridgeReply(true, success, stage, message, raw);
        }
        catch (Exception ex)
        {
            return new BridgeReply(false, false, "parse_error", ex.Message, raw);
        }
    }
}
