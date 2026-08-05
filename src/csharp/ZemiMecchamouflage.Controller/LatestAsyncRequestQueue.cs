namespace ZemiMecchamouflage.Controller;

/// <summary>
/// Serializes asynchronous configuration writes while coalescing queued work to
/// the newest requested value. An in-flight write is allowed to finish, but it
/// can never complete after a newer write and become the final applied value.
/// </summary>
public sealed class LatestAsyncRequestQueue<T>
{
    private readonly object gate = new();
    private readonly Func<T, Task<bool>> applyAsync;
    private readonly Action<Exception>? handleException;
    private readonly IEqualityComparer<T> comparer;
    private TaskCompletionSource idle =
        CompletedSource();
    private T? pending;
    private T? inFlight;
    private T? lastSuccessful;
    private bool hasPending;
    private bool hasInFlight;
    private bool hasLastSuccessful;
    private bool running;

    public LatestAsyncRequestQueue(
        Func<T, Task<bool>> applyAsync,
        Action<Exception>? handleException = null,
        IEqualityComparer<T>? comparer = null)
    {
        this.applyAsync = applyAsync ?? throw new ArgumentNullException(nameof(applyAsync));
        this.handleException = handleException;
        this.comparer = comparer ?? EqualityComparer<T>.Default;
    }

    public bool Enqueue(T value)
    {
        var startWorker = false;
        lock (gate)
        {
            if (hasPending && comparer.Equals(pending!, value))
                return false;
            if (!hasPending && hasInFlight && comparer.Equals(inFlight!, value))
                return false;
            if (!running && hasLastSuccessful && comparer.Equals(lastSuccessful!, value))
                return false;

            pending = value;
            hasPending = true;
            if (!running)
            {
                running = true;
                idle = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                startWorker = true;
            }
        }
        if (startWorker)
            _ = DrainAsync();
        return true;
    }

    public Task WhenIdleAsync()
    {
        lock (gate)
            return idle.Task;
    }

    private async Task DrainAsync()
    {
        while (true)
        {
            T value;
            lock (gate)
            {
                if (!hasPending)
                {
                    running = false;
                    idle.TrySetResult();
                    return;
                }
                value = pending!;
                pending = default;
                hasPending = false;
                inFlight = value;
                hasInFlight = true;
            }

            var succeeded = false;
            try
            {
                succeeded = await applyAsync(value).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                handleException?.Invoke(ex);
            }

            lock (gate)
            {
                inFlight = default;
                hasInFlight = false;
                if (succeeded)
                {
                    lastSuccessful = value;
                    hasLastSuccessful = true;
                }
                else
                {
                    lastSuccessful = default;
                    hasLastSuccessful = false;
                }
            }
        }
    }

    private static TaskCompletionSource CompletedSource()
    {
        var source = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        source.SetResult();
        return source;
    }
}
