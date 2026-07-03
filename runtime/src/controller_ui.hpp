#pragma once

#include "controller_settings.hpp"

#include <d3d11.h>
#include <string>

namespace meccha
{
    struct UiRuntimeState
    {
        std::string target_process{};
        std::string process_name{};
        unsigned long pid{0};
        std::string service_state{};
        std::string bridge_state{};
        bool bridge_ready{false};
        bool game_attached{false};
        bool paint_running{false};
        bool paint_ready{false};
        std::string paint_state{};
        std::string status_title{};
        std::string status_detail{};
        std::string mesh_status{};
        std::string planner_status{};
        std::string replay_status{};
        std::string metric_server_eta{};
        std::string metric_server_elapsed{};
        std::string metric_apply_eta{};
        std::string metric_apply_elapsed{};
        bool app_editing{false};
        bool paint_editing{false};
        bool recording_start_hotkey{false};
        bool recording_stop_hotkey{false};
        bool recording_preview_hotkey{false};
        bool recording_unpreview_hotkey{false};
        std::string hotkey_error{};
        std::string log_dir{};
    };

    struct UiActions
    {
        bool edit_app_clicked{false};
        bool cancel_app_clicked{false};
        bool save_app_clicked{false};
        bool reset_app_clicked{false};
        bool edit_paint_clicked{false};
        bool cancel_paint_clicked{false};
        bool save_paint_clicked{false};
        bool reset_paint_clicked{false};
        bool start_service_clicked{false};
        bool stop_service_clicked{false};
        bool minimize_clicked{false};
        bool close_clicked{false};
        bool open_logs_clicked{false};
        bool open_repository_clicked{false};
        bool open_license_clicked{false};
        bool copy_log_clicked{false};
        bool start_hotkey_recording{false};
        bool stop_hotkey_recording{false};
        bool preview_hotkey_recording{false};
        bool unpreview_hotkey_recording{false};
        bool settings_changed{false};
    };

    void apply_meccha_theme();
    void load_meccha_fonts();
    void initialize_meccha_ui_resources(ID3D11Device* device);
    void shutdown_meccha_ui_resources();
    void draw_app_ui(AppSettings& draft,
                     const AppSettings& persisted,
                     const UiRuntimeState& runtime,
                     const std::string& human_log_text,
                     UiActions& actions);
}
