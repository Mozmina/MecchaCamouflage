using System.Text.Json;
using MecchaCamouflage.Core;

var tests = new List<(string Name, Action Run)>
{
    ("legacy false region migrates to fill", LegacyFalseRegionMigratesToFill),
    ("payload includes fill material and region modes", PayloadIncludesFillMaterial),
    ("locales have complete keys", LocalesHaveCompleteKeys),
    ("color parser accepts rrggbb", ColorParserAcceptsHex),
    ("runtime cleanup removes old hash dirs", RuntimeCleanupRemovesOldHashDirs)
};

var failed = 0;
foreach (var test in tests)
{
    try
    {
        test.Run();
        Console.WriteLine($"PASS {test.Name}");
    }
    catch (Exception ex)
    {
        ++failed;
        Console.Error.WriteLine($"FAIL {test.Name}: {ex.Message}");
    }
}

return failed == 0 ? 0 : 1;

static void LegacyFalseRegionMigratesToFill()
{
    using var temp = new TempHome();
    var paths = new AppPaths("test-version");
    Directory.CreateDirectory(paths.VersionRoot);
    File.WriteAllText(paths.LegacyConfigPath, """
    {
      "layout_version": 23,
      "enable_front_paint": false,
      "enable_side_paint": true,
      "enable_back_paint": false
    }
    """);

    var settings = new SettingsStore(paths).Load();
    Assert(settings.Paint.FrontRegionMode == RegionMode.Fill, "front should migrate to fill");
    Assert(settings.Paint.SideRegionMode == RegionMode.Paint, "side should migrate to paint");
    Assert(settings.Paint.BackRegionMode == RegionMode.Fill, "back should migrate to fill");
}

static void PayloadIncludesFillMaterial()
{
    var settings = new AppSettings();
    settings.Paint.FrontRegionMode = RegionMode.Fill;
    settings.Paint.SideRegionMode = RegionMode.Skip;
    settings.Paint.BackRegionMode = RegionMode.Paint;
    settings.Paint.FillColor = new RgbColor(241, 17, 17);
    settings.Paint.FillMetallic = 1.0;
    settings.Paint.FillRoughness = 0.0;

    var payload = BridgePayloadBuilder.BuildPaintPayload(settings, 42, "Game.exe", new PaintRequestOptions());
    using var doc = JsonDocument.Parse(payload);
    var tuning = doc.RootElement.GetProperty("tuning");
    Assert(tuning.GetProperty("front_region_mode").GetString() == "fill", "front mode missing");
    Assert(tuning.GetProperty("side_region_mode").GetString() == "skip", "side mode missing");
    Assert(tuning.GetProperty("back_region_mode").GetString() == "paint", "back mode missing");
    Assert(tuning.GetProperty("fill_color").GetString() == "#F11111", "fill color missing");
    Assert(Math.Abs(tuning.GetProperty("fill_color_r").GetDouble() - (241.0 / 255.0)) < 0.00001, "fill red not normalized");
    Assert(tuning.GetProperty("enable_front_paint").GetBoolean() == false, "compat front bool wrong");
    Assert(tuning.GetProperty("enable_back_paint").GetBoolean(), "compat back bool wrong");
}

static void LocalesHaveCompleteKeys()
{
    var catalog = LocalizationCatalog.Load();
    var all = catalog.All;
    var englishKeys = all["en"].Keys.Order().ToArray();
    foreach (var locale in LocalizationCatalog.SupportedLocales)
    {
        Assert(all.ContainsKey(locale.Code), $"missing locale {locale.Code}");
        var keys = all[locale.Code].Keys.Order().ToArray();
        Assert(englishKeys.SequenceEqual(keys), $"key mismatch for {locale.Code}");
    }
}

static void ColorParserAcceptsHex()
{
    Assert(RgbColor.TryParse("F11111", out var color), "hex without # should parse");
    Assert(color.ToHex() == "#F11111", "hex roundtrip failed");
}

static void RuntimeCleanupRemovesOldHashDirs()
{
    using var temp = new TempHome();
    var paths = new AppPaths("cleanup-test");
    var keep = Path.Combine(paths.RuntimeBinDirectory, "keep");
    var recent = Path.Combine(paths.RuntimeBinDirectory, "recent");
    var old = Path.Combine(paths.RuntimeBinDirectory, "old");
    Directory.CreateDirectory(keep);
    Directory.CreateDirectory(recent);
    Directory.CreateDirectory(old);
    File.WriteAllText(Path.Combine(keep, "current.dll"), "");
    File.WriteAllText(Path.Combine(recent, "bridge.dll"), "");
    File.WriteAllText(Path.Combine(old, "bridge.dll"), "");
    Directory.SetLastWriteTimeUtc(old, DateTime.UtcNow - TimeSpan.FromDays(30));

    paths.CleanupRuntimeBinDirectories(keep, TimeSpan.FromDays(14), keepNewest: 3);

    Assert(Directory.Exists(keep), "current hash dir should be kept");
    Assert(Directory.Exists(recent), "recent hash dir should be kept");
    Assert(!Directory.Exists(old), "old hash dir should be removed");
}

static void Assert(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

sealed class TempHome : IDisposable
{
    private readonly string oldLocalAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
    private readonly string temp = Path.Combine(Path.GetTempPath(), "meccha-tests-" + Guid.NewGuid().ToString("N"));

    public TempHome()
    {
        Directory.CreateDirectory(temp);
        Environment.SetEnvironmentVariable("LOCALAPPDATA", temp);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable("LOCALAPPDATA", oldLocalAppData);
        try { Directory.Delete(temp, true); } catch { }
    }
}
