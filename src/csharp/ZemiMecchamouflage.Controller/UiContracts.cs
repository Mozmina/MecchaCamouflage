using ZemiMecchamouflage.Core;
using System.Text.Json;

namespace ZemiMecchamouflage.Controller;

public sealed record UiSnapshot(
    string Version,
    string Language,
    RuntimeSnapshot Runtime,
    SettingsSnapshot Settings,
    SettingsSnapshot Defaults,
    ResetSnapshot ResetState,
    IReadOnlyList<LocaleSnapshot> Locales,
    IReadOnlyDictionary<string, IReadOnlyDictionary<string, string>> Translations);

public sealed record RuntimeSnapshot(
    string Process,
    string Bridge,
    string Service,
    double ProgressPercent,
    string PaintProgressSource,
    string PaintPass,
    string PaintPassProgress,
    string PaintPassEta,
    string PaintEta,
    string PaintElapsed,
    string Logs,
    bool PaintRunning,
    bool ProgressVisible,
    DiagnosticsSnapshot Diagnostics);

public sealed record SettingsSnapshot(PaintSnapshot Paint, AppSnapshot App);

public sealed record PaintSnapshot(
    double BrushSizeTexels,
    bool AutoMaterial,
    double Metallic,
    double Roughness,
    double Emissive,
    string FrontRegionMode,
    string SideRegionMode,
    string BackRegionMode,
    string FillColor,
    double FillMetallic,
    double FillRoughness,
    double FillEmissive,
    bool UsesFill,
    double ColorCompressionTolerance = 0.0,
    double SecondPassBrushSizeTexels = 1.0,
    double SecondPassColorCompressionTolerance = 0.0);

public sealed record AppSnapshot(
    string ProcessName,
    bool AlwaysOnTop,
    double Opacity,
    string ThemeColor,
    string StartHotkey,
    string PreviewHotkey,
    string UnPreviewHotkey,
    string StopHotkey,
    string SecondPassHotkey = "F5");

public sealed record ResetSnapshot(
    IReadOnlyDictionary<string, bool> Settings,
    IReadOnlyDictionary<string, bool> Sections);

public sealed record LocaleSnapshot(string Code, string NativeName);

public sealed record HostCommandResult(bool Success, string Message = "");

public sealed record SettingChange(string Key, JsonElement Value);

public sealed record ProgressSnapshot(
    string Phase,
    string Result,
    bool Terminal,
    int Step,
    int TotalSteps,
    double Progress,
    double PaintEtaMs,
    double PaintElapsedMs,
    string ReplayCurrentPass = "",
    int ReplayCurrentPassStart = -1,
    int ReplayCurrentPassEnd = -1,
    string ReplayProgressSource = "",
    int ReplayCurrentPassCompleted = -1,
    int ReplayCurrentPassTotal = -1,
    double ReplayCurrentPassEtaMs = -1.0);

public sealed record HotkeySet(string Start, string Preview, string UnPreview, string Stop, string SecondPass)
{
    public static HotkeySet From(AppSettings settings) =>
        new(settings.StartHotkey, settings.PreviewHotkey, settings.UnPreviewHotkey, settings.StopHotkey, settings.SecondPassHotkey);

    public void ApplyTo(AppSettings settings)
    {
        settings.StartHotkey = Start;
        settings.PreviewHotkey = Preview;
        settings.UnPreviewHotkey = UnPreview;
        settings.StopHotkey = Stop;
        settings.SecondPassHotkey = SecondPass;
    }

    public bool TryValidate(out string message)
    {
        message = "";
        var values = new[] { Start, Preview, UnPreview, Stop, SecondPass };
        foreach (var value in values)
        {
            if (!IsFunctionKey(value))
            {
                message = "Hotkeys must be F1 through F24.";
                return false;
            }
        }
        if (values.Select(Normalize).Distinct(StringComparer.OrdinalIgnoreCase).Count() != values.Length)
        {
            message = "Hotkeys must not be duplicated.";
            return false;
        }
        return true;
    }

    public static bool IsFunctionKey(string? value)
    {
        var normalized = Normalize(value);
        return normalized.Length >= 2 &&
               normalized[0] == 'F' &&
               int.TryParse(normalized[1..], out var number) &&
               number is >= 1 and <= 24;
    }

    public static string Normalize(string? value) => (value ?? "").Trim().ToUpperInvariant();
}

public sealed class HotkeyKeyState
{
    private readonly HashSet<uint> pressedKeys = [];

    public bool TryBeginPress(uint virtualKey) => pressedKeys.Add(virtualKey);

    public void EndPress(uint virtualKey) => pressedKeys.Remove(virtualKey);

    public void Clear() => pressedKeys.Clear();
}
