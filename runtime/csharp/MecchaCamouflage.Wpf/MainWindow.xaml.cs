using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Navigation;
using System.Windows.Threading;
using MecchaCamouflage.Core;

namespace MecchaCamouflage.Wpf;

public partial class MainWindow : Window
{
    private readonly LocalizationCatalog localization = LocalizationCatalog.Load();
    private readonly AppPaths paths = new(VersionInfo.Current);
    private readonly SettingsStore store;
    private readonly RuntimeLog log;
    private readonly RuntimeBridgeService runtime;
    private readonly DispatcherTimer statusTimer = new();
    private HotkeyManager? hotkeys;
    private AppSettings settings;
    private AppSettings draft;
    private bool editing;
    private bool updatingUi;
    private bool paintRunning;

    public MainWindow()
    {
        InitializeComponent();
        store = new SettingsStore(paths);
        settings = store.Load();
        draft = Clone(settings);
        log = new RuntimeLog(paths);
        runtime = new RuntimeBridgeService(paths, log);
        Width = settings.PanelWidth;
        Height = settings.PanelHeight;
        if (settings.PanelX >= 0 && settings.PanelY >= 0)
        {
            Left = settings.PanelX;
            Top = settings.PanelY;
        }
        Topmost = settings.AlwaysOnTop;
        Opacity = settings.Opacity;
        SourceInitialized += (_, _) =>
        {
            hotkeys = new HotkeyManager(this);
            RegisterHotkeys();
        };
        Closed += (_, _) =>
        {
            hotkeys?.Dispose();
            _ = runtime.PingAsync().ContinueWith(task =>
            {
                if (task.Result.Ok)
                    _ = new BridgeClient(port: RuntimeBridgeService.BridgePort).ShutdownAsync();
            });
        };
        InitializeLanguageSelector();
        ApplyLocalization();
        ApplySettingsToUi(settings);
        ApplyEditingState();
        log.Info("Runtime start desktop app started");
        UpdateLogText();

        statusTimer.Interval = TimeSpan.FromSeconds(2);
        statusTimer.Tick += async (_, _) => await UpdateStatusAsync();
        statusTimer.Start();
        _ = UpdateStatusAsync();
    }

    private string T(string key) => localization.Text(settings.Language, key);

    private void InitializeLanguageSelector()
    {
        LanguageBox.ItemsSource = LocalizationCatalog.SupportedLocales;
        LanguageBox.DisplayMemberPath = nameof(LocaleInfo.NativeName);
        LanguageBox.SelectedValuePath = nameof(LocaleInfo.Code);
    }

    private void ApplyLocalization()
    {
        updatingUi = true;
        Title = T("app.title");
        TitleText.Text = T("app.title");
        FooterVersionText.Text = VersionInfo.Current;
        FooterCopyrightRun.Text = $"(c) {DateTime.Now.Year} acentrist. GPL-3.0-or-later.";
        PaintSettingsHeader.Text = T("settings.paint");
        BrushLabel.Text = T("brush.size");
        CoverageLabel.Text = T("coverage.step");
        DelayLabel.Text = T("stroke.delay");
        AutoMaterialBox.Content = T("auto.material");
        MetallicLabel.Text = T("metallic");
        RoughnessLabel.Text = T("roughness");
        RegionsHeader.Text = T("regions");
        FrontLabel.Text = T("region.front");
        SidesLabel.Text = T("region.sides");
        BackLabel.Text = T("region.back");
        FillMaterialHeader.Text = T("fill.material");
        FillColorLabel.Text = T("fill.color");
        FillMetallicLabel.Text = T("fill.metallic");
        FillRoughnessLabel.Text = T("fill.roughness");
        AppSettingsHeader.Text = T("settings.app");
        LanguageLabel.Text = T("language");
        ProcessNameLabel.Text = T("process.name");
        StartHotkeyLabel.Text = T("start.hotkey");
        PreviewHotkeyLabel.Text = T("preview.hotkey");
        UnPreviewHotkeyLabel.Text = T("unpreview.hotkey");
        StopHotkeyLabel.Text = T("stop.hotkey");
        AlwaysOnTopBox.Content = T("always.on.top");
        OpacityLabel.Text = T("opacity");
        EditButton.Content = T("button.edit");
        SaveButton.Content = T("button.save");
        CancelButton.Content = T("button.cancel");
        ResetButton.Content = T("button.reset");
        StartButton.Content = T("button.start");
        StopButton.Content = T("button.stop");
        OpenLogsButton.Content = T("button.open.logs");
        CopyLogButton.Content = T("button.copy.log");
        ProcessStatusLabel.Text = T("status.process");
        BridgeStatusLabel.Text = T("status.bridge");
        ServiceStatusLabel.Text = T("status.service");
        PaintStatusLabel.Text = "Progress";
        LogsHeader.Text = T("status.logs");
        PaintEtaLabel.Text = T("metric.paint.eta");
        PaintElapsedLabel.Text = T("metric.paint.elapsed");
        FrontPaintRadio.Content = T("mode.paint");
        FrontFillRadio.Content = T("mode.fill");
        FrontSkipRadio.Content = T("mode.skip");
        SidePaintRadio.Content = T("mode.paint");
        SideFillRadio.Content = T("mode.fill");
        SideSkipRadio.Content = T("mode.skip");
        BackPaintRadio.Content = T("mode.paint");
        BackFillRadio.Content = T("mode.fill");
        BackSkipRadio.Content = T("mode.skip");
        LanguageBox.SelectedValue = settings.Language;
        updatingUi = false;
    }

    private PaintSettings CurrentPaint() => editing ? draft.Paint : settings.Paint;

    private static void SelectRegionMode(RegionMode mode, RadioButton paint, RadioButton fill, RadioButton skip)
    {
        paint.IsChecked = mode == RegionMode.Paint;
        fill.IsChecked = mode == RegionMode.Fill;
        skip.IsChecked = mode == RegionMode.Skip;
    }

    private static RegionMode SelectedRegionMode(RadioButton paint, RadioButton fill, RadioButton skip)
    {
        if (fill.IsChecked == true)
            return RegionMode.Fill;
        if (skip.IsChecked == true)
            return RegionMode.Skip;
        return RegionMode.Paint;
    }

    private void ApplySettingsToUi(AppSettings source)
    {
        updatingUi = true;
        BrushTextBox.Text = source.Paint.StrokeSizeTexels.ToString("0.###", CultureInfo.InvariantCulture);
        CoverageTextBox.Text = source.Paint.CoverageStepTexels.ToString("0.###", CultureInfo.InvariantCulture);
        DelayTextBox.Text = source.Paint.ServerBatchDelayMs.ToString(CultureInfo.InvariantCulture);
        AutoMaterialBox.IsChecked = source.Paint.AutoMaterial;
        MetallicTextBox.Text = source.Paint.Metallic.ToString("0.######", CultureInfo.InvariantCulture);
        RoughnessTextBox.Text = source.Paint.Roughness.ToString("0.######", CultureInfo.InvariantCulture);
        SelectRegionMode(source.Paint.FrontRegionMode, FrontPaintRadio, FrontFillRadio, FrontSkipRadio);
        SelectRegionMode(source.Paint.SideRegionMode, SidePaintRadio, SideFillRadio, SideSkipRadio);
        SelectRegionMode(source.Paint.BackRegionMode, BackPaintRadio, BackFillRadio, BackSkipRadio);
        FillColorTextBox.Text = source.Paint.FillColor.ToHex();
        FillMetallicTextBox.Text = source.Paint.FillMetallic.ToString("0.######", CultureInfo.InvariantCulture);
        FillRoughnessTextBox.Text = source.Paint.FillRoughness.ToString("0.######", CultureInfo.InvariantCulture);
        LanguageBox.SelectedValue = source.Language;
        ProcessNameTextBox.Text = source.GameProcessName;
        StartHotkeyTextBox.Text = source.StartHotkey;
        PreviewHotkeyTextBox.Text = source.PreviewHotkey;
        UnPreviewHotkeyTextBox.Text = source.UnPreviewHotkey;
        StopHotkeyTextBox.Text = source.StopHotkey;
        AlwaysOnTopBox.IsChecked = source.AlwaysOnTop;
        OpacitySlider.Value = source.Opacity * 100.0;
        FillMaterialPanel.Visibility = source.Paint.UsesFill ? Visibility.Visible : Visibility.Collapsed;
        MetallicTextBox.IsEnabled = editing && !source.Paint.AutoMaterial;
        RoughnessTextBox.IsEnabled = editing && !source.Paint.AutoMaterial;
        updatingUi = false;
    }

    private bool CollectUiTo(AppSettings target)
    {
        target.Paint.StrokeSizeTexels = ReadDouble(BrushTextBox, target.Paint.StrokeSizeTexels);
        target.Paint.CoverageStepTexels = ReadDouble(CoverageTextBox, target.Paint.CoverageStepTexels);
        target.Paint.ServerBatchDelayMs = ReadInt(DelayTextBox, target.Paint.ServerBatchDelayMs);
        target.Paint.AutoMaterial = AutoMaterialBox.IsChecked == true;
        target.Paint.Metallic = ReadDouble(MetallicTextBox, target.Paint.Metallic);
        target.Paint.Roughness = ReadDouble(RoughnessTextBox, target.Paint.Roughness);
        target.Paint.FrontRegionMode = SelectedRegionMode(FrontPaintRadio, FrontFillRadio, FrontSkipRadio);
        target.Paint.SideRegionMode = SelectedRegionMode(SidePaintRadio, SideFillRadio, SideSkipRadio);
        target.Paint.BackRegionMode = SelectedRegionMode(BackPaintRadio, BackFillRadio, BackSkipRadio);
        if (!RgbColor.TryParse(FillColorTextBox.Text, out var fill))
        {
            MessageBox.Show(this, T("validation.color"), T("app.title"), MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }
        target.Paint.FillColor = fill;
        target.Paint.FillMetallic = ReadDouble(FillMetallicTextBox, target.Paint.FillMetallic);
        target.Paint.FillRoughness = ReadDouble(FillRoughnessTextBox, target.Paint.FillRoughness);
        target.Language = LanguageBox.SelectedValue as string ?? target.Language;
        target.GameProcessName = ProcessNameTextBox.Text.Trim();
        target.StartHotkey = StartHotkeyTextBox.Text.Trim();
        target.PreviewHotkey = PreviewHotkeyTextBox.Text.Trim();
        target.UnPreviewHotkey = UnPreviewHotkeyTextBox.Text.Trim();
        target.StopHotkey = StopHotkeyTextBox.Text.Trim();
        target.AlwaysOnTop = AlwaysOnTopBox.IsChecked == true;
        target.Opacity = OpacitySlider.Value / 100.0;
        target.PanelWidth = ActualWidth > 0 ? ActualWidth : Width;
        target.PanelHeight = ActualHeight > 0 ? ActualHeight : Height;
        target.PanelX = Left;
        target.PanelY = Top;
        return true;
    }

    private void ApplyEditingState()
    {
        foreach (var control in new Control[]
        {
            BrushTextBox, CoverageTextBox, DelayTextBox, AutoMaterialBox, MetallicTextBox, RoughnessTextBox,
            FrontPaintRadio, FrontFillRadio, FrontSkipRadio,
            SidePaintRadio, SideFillRadio, SideSkipRadio,
            BackPaintRadio, BackFillRadio, BackSkipRadio,
            FillColorTextBox, FillMetallicTextBox, FillRoughnessTextBox,
            ProcessNameTextBox, StartHotkeyTextBox, PreviewHotkeyTextBox, UnPreviewHotkeyTextBox, StopHotkeyTextBox,
            AlwaysOnTopBox, OpacitySlider
        })
        {
            control.IsEnabled = editing;
        }
        LanguageBox.IsEnabled = true;
        EditButton.IsEnabled = !editing;
        SaveButton.IsEnabled = editing;
        CancelButton.IsEnabled = editing;
        ResetButton.IsEnabled = editing;
        MetallicTextBox.IsEnabled = editing && AutoMaterialBox.IsChecked != true;
        RoughnessTextBox.IsEnabled = editing && AutoMaterialBox.IsChecked != true;
        FillMaterialPanel.Visibility = CurrentPaint().UsesFill ? Visibility.Visible : Visibility.Collapsed;
    }

    private async Task UpdateStatusAsync()
    {
        var process = runtime.FindGameProcess(settings.GameProcessName);
        ProcessStatusValue.Text = process is null ? T("state.waiting") : T("state.attached");
        ProcessStatusValue.Foreground = process is null ? Brushes.Goldenrod : Brushes.LightGreen;
        var ping = await runtime.PingAsync();
        BridgeStatusValue.Text = ping.Ok && ping.Success ? T("state.ready") : T("state.waiting");
        BridgeStatusValue.Foreground = ping.Ok && ping.Success ? Brushes.LightGreen : Brushes.Goldenrod;
        ServiceStatusValue.Text = ping.Ok && ping.Success ? T("state.ready") : T("state.stopped");
        var progress = ReadProgressSnapshot(runtime.ProgressPath);
        if (progress is null)
        {
            PaintStatusValue.Text = paintRunning ? "0%" : "-";
            PaintEtaValue.Text = "-";
            PaintElapsedValue.Text = "-";
        }
        else
        {
            var percent = progress.TotalSteps > 0 ? Math.Clamp(progress.Step * 100.0 / progress.TotalSteps, 0.0, 100.0) : progress.Progress * 100.0;
            PaintStatusValue.Text = $"{percent:0}%";
            PaintEtaValue.Text = FormatDuration(progress.PaintEtaMs);
            PaintElapsedValue.Text = FormatDuration(progress.PaintElapsedMs);
        }
        UpdateLogText();
    }

    private async Task RunPaintAsync(bool previewOnly, bool unpreviewOnly)
    {
        if (editing)
        {
            log.Warn("Paint ignored because settings are being edited.");
            UpdateLogText();
            return;
        }
        if (paintRunning)
            return;
        paintRunning = true;
        PaintStatusValue.Text = "0%";
        try
        {
            var ready = await runtime.EnsureReadyAsync(settings.GameProcessName);
            if (!ready)
            {
                log.Warn("Bridge not ready.");
                return;
            }
            var process = runtime.FindGameProcess(settings.GameProcessName);
            if (process is null)
            {
                log.Warn("Game process not found.");
                return;
            }
            log.Info(previewOnly ? "Preview triggered." : (unpreviewOnly ? "UnPreview triggered." : "Paint triggered."));
            var payload = BridgePayloadBuilder.BuildPaintPayload(settings,
                                                                 process.Id,
                                                                 settings.GameProcessName,
                                                                 new PaintRequestOptions(previewOnly, unpreviewOnly, Environment.GetEnvironmentVariable("MECCHA_RESEARCH_ARTIFACTS") == "1"));
            var response = await runtime.SendPaintAsync(payload);
            if (response.Success)
                log.Info(response.Message.Length > 0 ? response.Message : response.Stage);
            else
                log.Error(response.Message.Length > 0 ? response.Message : response.Stage);
        }
        finally
        {
            paintRunning = false;
            await UpdateStatusAsync();
        }
    }

    private void UpdateLogText()
    {
        LogTextBox.Text = string.IsNullOrWhiteSpace(log.Text) ? T("logs.empty") : log.Text;
        LogTextBox.ScrollToEnd();
    }

    private void RegisterHotkeys()
    {
        hotkeys?.Register(settings.StartHotkey,
                          settings.PreviewHotkey,
                          settings.UnPreviewHotkey,
                          settings.StopHotkey,
                          () => Dispatcher.Invoke(async () => await RunPaintAsync(false, false)),
                          () => Dispatcher.Invoke(async () => await RunPaintAsync(true, false)),
                          () => Dispatcher.Invoke(async () => await RunPaintAsync(false, true)),
                          () => Dispatcher.Invoke(async () => await StopPaintAsync()));
    }

    private async Task StopPaintAsync()
    {
        var response = await runtime.CancelPaintAsync();
        log.Info(response.Success ? "Paint cancel requested." : "Paint cancel failed: " + response.Message);
        UpdateLogText();
        paintRunning = false;
        await UpdateStatusAsync();
    }

    private void EditClicked(object sender, RoutedEventArgs e)
    {
        draft = Clone(settings);
        editing = true;
        ApplySettingsToUi(draft);
        ApplyEditingState();
    }

    private void SaveClicked(object sender, RoutedEventArgs e)
    {
        if (!CollectUiTo(draft))
            return;
        settings = SettingsStore.Clamp(Clone(draft));
        store.Save(settings);
        Topmost = settings.AlwaysOnTop;
        Opacity = settings.Opacity;
        editing = false;
        RegisterHotkeys();
        ApplyLocalization();
        ApplySettingsToUi(settings);
        ApplyEditingState();
        log.Info("Settings saved.");
        UpdateLogText();
    }

    private void CancelClicked(object sender, RoutedEventArgs e)
    {
        draft = Clone(settings);
        editing = false;
        ApplySettingsToUi(settings);
        ApplyEditingState();
    }

    private void ResetClicked(object sender, RoutedEventArgs e)
    {
        draft = new AppSettings { Language = settings.Language };
        ApplySettingsToUi(draft);
    }

    private async void StartClicked(object sender, RoutedEventArgs e) => await RunPaintAsync(false, false);
    private async void StopClicked(object sender, RoutedEventArgs e) => await StopPaintAsync();

    private void OpenLogsClicked(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(paths.LogDirectory);
        Process.Start(new ProcessStartInfo(paths.LogDirectory) { UseShellExecute = true });
    }

    private void CopyLogClicked(object sender, RoutedEventArgs e)
    {
        Clipboard.SetText(LogTextBox.Text);
        log.Info("Log copied.");
        UpdateLogText();
    }

    private void FooterLinkNavigate(object sender, RequestNavigateEventArgs e)
    {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
        e.Handled = true;
    }

    private void LanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (updatingUi || LanguageBox.SelectedValue is not string code)
            return;
        if (editing)
            draft.Language = code;
        settings.Language = code;
        store.Save(settings);
        ApplyLocalization();
        ApplySettingsToUi(editing ? draft : settings);
        ApplyEditingState();
    }

    private void RegionRadioChanged(object sender, RoutedEventArgs e)
    {
        if (updatingUi)
            return;
        var paint = CurrentPaint();
        paint.FrontRegionMode = SelectedRegionMode(FrontPaintRadio, FrontFillRadio, FrontSkipRadio);
        paint.SideRegionMode = SelectedRegionMode(SidePaintRadio, SideFillRadio, SideSkipRadio);
        paint.BackRegionMode = SelectedRegionMode(BackPaintRadio, BackFillRadio, BackSkipRadio);
        FillMaterialPanel.Visibility = paint.UsesFill ? Visibility.Visible : Visibility.Collapsed;
    }

    private static double ReadDouble(TextBox box, double fallback) =>
        double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : fallback;

    private static int ReadInt(TextBox box, int fallback) =>
        int.TryParse(box.Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ? value : fallback;

    private static AppSettings Clone(AppSettings source) =>
        JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(source)) ?? new AppSettings();

    private static ProgressSnapshot? ReadProgressSnapshot(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            return null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var root = doc.RootElement;
            return new ProgressSnapshot(
                Text(root, "phase", Text(root, "stage", "")),
                Text(root, "result", ""),
                Bool(root, "terminal", false),
                Int(root, "step", 0),
                Int(root, "total_steps", Int(root, "total_strokes", 0)),
                Number(root, "progress", 0.0),
                Number(root, "paint_eta_ms", -1.0),
                Number(root, "paint_elapsed_ms", Number(root, "elapsed_ms", -1.0)));
        }
        catch
        {
            return null;
        }
    }

    private static string FormatDuration(double milliseconds)
    {
        if (!double.IsFinite(milliseconds) || milliseconds < 0.0)
            return "-";
        if (milliseconds < 1000.0)
            return "<1s";
        var totalSeconds = (int)Math.Round(milliseconds / 1000.0);
        if (totalSeconds < 60)
            return totalSeconds + "s";
        var minutes = totalSeconds / 60;
        var seconds = totalSeconds % 60;
        if (minutes < 60)
            return $"{minutes}m {seconds:00}s";
        var hours = minutes / 60;
        minutes %= 60;
        return $"{hours}h {minutes:00}m";
    }

    private static string Text(JsonElement root, string key, string fallback) =>
        root.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? fallback : fallback;

    private static bool Bool(JsonElement root, string key, bool fallback) =>
        root.TryGetProperty(key, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False ? value.GetBoolean() : fallback;

    private static int Int(JsonElement root, string key, int fallback) =>
        root.TryGetProperty(key, out var value) && value.TryGetInt32(out var parsed) ? parsed : fallback;

    private static double Number(JsonElement root, string key, double fallback) =>
        root.TryGetProperty(key, out var value) && value.TryGetDouble(out var parsed) ? parsed : fallback;

    private sealed record ProgressSnapshot(
        string Phase,
        string Result,
        bool Terminal,
        int Step,
        int TotalSteps,
        double Progress,
        double PaintEtaMs,
        double PaintElapsedMs);
}
