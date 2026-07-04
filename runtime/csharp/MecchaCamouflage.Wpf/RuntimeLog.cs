using MecchaCamouflage.Core;

namespace MecchaCamouflage.Wpf;

public sealed class RuntimeLog
{
    private readonly AppPaths paths;
    private readonly List<string> lines = [];

    public RuntimeLog(AppPaths paths)
    {
        this.paths = paths;
        Directory.CreateDirectory(paths.LogDirectory);
    }

    public string Text => lines.Count == 0 ? "" : string.Join(Environment.NewLine, lines);

    public void Info(string message) => Write("INFO", message);
    public void Warn(string message) => Write("WARN", message);
    public void Error(string message) => Write("ERROR", message);

    private void Write(string level, string message)
    {
        var line = $"{DateTime.Now:HH:mm:ss} [{level}] {message}";
        lines.Add(line);
        while (lines.Count > 400)
            lines.RemoveAt(0);
        var path = Path.Combine(paths.LogDirectory, $"runtime-{DateTime.Now:yyyy-MM-dd}.log");
        File.AppendAllText(path, line + Environment.NewLine);
    }
}
