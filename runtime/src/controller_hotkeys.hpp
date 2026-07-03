#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace meccha
{
    struct HotkeyBinding
    {
        UINT vk{VK_F10};
        UINT modifiers{0};
    };

    struct OverlayHotkeyState
    {
        bool start_requested{false};
        bool stop_requested{false};
        bool preview_requested{false};
        bool unpreview_requested{false};
    };

    auto parse_hotkey_binding(const std::string& text, UINT default_vk = VK_F10) -> HotkeyBinding;
    auto hotkey_to_string(const HotkeyBinding& binding) -> std::string;
    auto hotkey_backend_json(const HotkeyBinding& start,
                             bool start_registered,
                             const HotkeyBinding& stop,
                             bool stop_registered,
                             const HotkeyBinding& preview,
                             bool preview_registered,
                             const HotkeyBinding& unpreview,
                             bool unpreview_registered) -> std::string;
    auto try_capture_hotkey_from_message(const MSG& msg, HotkeyBinding& out, std::string& error, bool& cancel) -> bool;

    class OverlayHotkeys
    {
    public:
        OverlayHotkeys(HotkeyBinding start, HotkeyBinding stop, HotkeyBinding preview, HotkeyBinding unpreview);
        ~OverlayHotkeys();

        auto set_start_hotkey(HotkeyBinding start, std::string* error = nullptr) -> bool;
        auto set_stop_hotkey(HotkeyBinding stop, std::string* error = nullptr) -> bool;
        auto set_hotkeys(HotkeyBinding start, HotkeyBinding stop, HotkeyBinding preview, HotkeyBinding unpreview, std::string* error = nullptr) -> bool;
        auto backend_json() const -> std::string;
        auto start_binding() const -> HotkeyBinding { return start_; }
        auto stop_binding() const -> HotkeyBinding { return stop_; }
        auto preview_binding() const -> HotkeyBinding { return preview_; }
        auto unpreview_binding() const -> HotkeyBinding { return unpreview_; }
        auto start_registered() const -> bool { return start_registered_; }
        auto stop_registered() const -> bool { return stop_registered_; }
        auto preview_registered() const -> bool { return preview_registered_; }
        auto unpreview_registered() const -> bool { return unpreview_registered_; }
        void handle_message(const MSG& msg, OverlayHotkeyState& state) const;
        void poll_fallback(OverlayHotkeyState& state);

    private:
        void unregister_start();
        void unregister_stop();
        void unregister_preview();
        void unregister_unpreview();

        HotkeyBinding start_{};
        HotkeyBinding stop_{VK_F9, 0};
        HotkeyBinding preview_{VK_F8, 0};
        HotkeyBinding unpreview_{VK_F7, 0};
        bool start_registered_{false};
        bool stop_registered_{false};
        bool preview_registered_{false};
        bool unpreview_registered_{false};
        bool start_down_{false};
        bool stop_down_{false};
        bool preview_down_{false};
        bool unpreview_down_{false};
    };
}
