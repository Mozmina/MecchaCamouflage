#pragma once

#include <filesystem>
#include <string>

namespace meccha
{
    inline constexpr wchar_t DefaultGameProcessName[] = L"PenguinHotel-Win64-Shipping.exe";

    struct PaintTuning
    {
        double stroke_size_texels{9.0};
        double coverage_step_texels{9.0};
        double side_source_max_uv{0.08};
        double front_back_source_max_uv{0.45};
        bool enable_front_paint{true};
        bool enable_side_paint{true};
        bool enable_back_paint{true};
        int server_batch_limit{1};
        int server_batch_delay_ms{1};
        bool auto_material_properties{true};
        double metallic{0.0};
        double roughness{1.0};
    };

    struct AppSettings
    {
        int layout_version{22};
        float panel_x{-1.0f};
        float panel_y{-1.0f};
        float panel_width{1040.0f};
        float panel_height{640.0f};
        int log_retention_days{14};
        std::wstring game_process_name{DefaultGameProcessName};
        bool always_on_top{true};
        float opacity{1.0f};
        std::string start_hotkey{"F10"};
        std::string stop_hotkey{"F9"};
        std::string preview_hotkey{"F8"};
        std::string unpreview_hotkey{"F7"};
        PaintTuning tuning{};
        bool show_info{true};
        bool show_warning{true};
        bool show_error{true};
    };

    auto default_app_dir() -> std::filesystem::path;
    auto config_path() -> std::filesystem::path;
    auto app_version() -> std::string;
    auto default_tuning() -> PaintTuning;
    void clamp_settings(AppSettings& settings);
    auto load_settings() -> AppSettings;
    auto save_settings(const AppSettings& settings) -> bool;
}
