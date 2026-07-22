using System.Globalization;
using System.Text.Json;

namespace ZemiMecchamouflage.Core;

public sealed record PaintRequestOptions(
    bool PreviewOnly = false,
    bool UnPreviewOnly = false,
    bool ResearchArtifacts = false,
    int DiagnosticStrokeLimit = 0);

public static class BridgePayloadBuilder
{
    public static string BuildPaintPayload(AppSettings settings, int processId, string processName, PaintRequestOptions options)
    {
        var paint = SettingsStore.Clamp(settings).Paint;
        var payload = new Dictionary<string, object?>
        {
            ["type"] = "paint_full_route",
            ["native_apply_mode"] = "native_recorded_paint",
            ["route"] = "native_recorded_paint",
            ["preview_only"] = options.PreviewOnly,
            ["unpreview_only"] = options.UnPreviewOnly,
            ["research_artifacts"] = options.ResearchArtifacts,
            ["process"] = new Dictionary<string, object?>
            {
                ["pid"] = processId,
                ["name"] = processName
            },
            ["tuning"] = new Dictionary<string, object?>
            {
                ["brush_size_texels"] = paint.BrushSizeTexels,
                ["side_source_max_uv"] = paint.SideSourceMaxUv,
                ["front_back_source_max_uv"] = paint.FrontBackSourceMaxUv,
                ["auto_material"] = paint.AutoMaterial,
                ["metallic"] = paint.Metallic,
                ["roughness"] = paint.Roughness,
                ["emissive"] = paint.Emissive,
                ["front_region_mode"] = SettingsStore.RegionModeText(paint.FrontRegionMode),
                ["side_region_mode"] = SettingsStore.RegionModeText(paint.SideRegionMode),
                ["back_region_mode"] = SettingsStore.RegionModeText(paint.BackRegionMode),
                ["fill_color"] = paint.FillColor.ToHex(),
                ["fill_color_r"] = ToUnit(paint.FillColor.R),
                ["fill_color_g"] = ToUnit(paint.FillColor.G),
                ["fill_color_b"] = ToUnit(paint.FillColor.B),
                ["fill_metallic"] = paint.FillMetallic,
                ["fill_roughness"] = paint.FillRoughness,
                ["fill_emissive"] = paint.FillEmissive,
                ["color_compression_tolerance"] = paint.ColorCompressionTolerance
            }
        };
        if (options.DiagnosticStrokeLimit > 0)
            payload["diagnostic_stroke_limit"] = Math.Clamp(options.DiagnosticStrokeLimit, 1, 10_000);
        return JsonSerializer.Serialize(payload) + "\n";
    }

    private static double ToUnit(byte value) =>
        double.Parse((value / 255.0).ToString("0.########", CultureInfo.InvariantCulture), CultureInfo.InvariantCulture);
}
