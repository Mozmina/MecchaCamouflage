#include "controller_hotkeys.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace meccha
{
    namespace
    {
        auto upper_copy(std::string value) -> std::string
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return value;
        }

        auto is_modifier_vk(UINT vk) -> bool
        {
            return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                   vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                   vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                   vk == VK_LWIN || vk == VK_RWIN;
        }

        auto is_function_vk(UINT vk) -> bool
        {
            return vk >= VK_F1 && vk <= VK_F24;
        }

        auto vk_label(UINT vk) -> std::string
        {
            if (vk >= 'A' && vk <= 'Z')
                return std::string(1, static_cast<char>(vk));
            if (vk >= '0' && vk <= '9')
                return std::string(1, static_cast<char>(vk));
            if (vk >= VK_F1 && vk <= VK_F24)
                return "F" + std::to_string(vk - VK_F1 + 1);
            switch (vk)
            {
            case VK_SPACE: return "Space";
            case VK_TAB: return "Tab";
            case VK_RETURN: return "Enter";
            case VK_BACK: return "Backspace";
            case VK_DELETE: return "Delete";
            case VK_INSERT: return "Insert";
            case VK_HOME: return "Home";
            case VK_END: return "End";
            case VK_PRIOR: return "PageUp";
            case VK_NEXT: return "PageDown";
            case VK_LEFT: return "Left";
            case VK_RIGHT: return "Right";
            case VK_UP: return "Up";
            case VK_DOWN: return "Down";
            case VK_OEM_PLUS: return "Plus";
            case VK_OEM_MINUS: return "Minus";
            case VK_OEM_COMMA: return "Comma";
            case VK_OEM_PERIOD: return "Period";
            case VK_OEM_1: return "Semicolon";
            case VK_OEM_2: return "Slash";
            case VK_OEM_3: return "Backquote";
            case VK_OEM_4: return "LeftBracket";
            case VK_OEM_5: return "Backslash";
            case VK_OEM_6: return "RightBracket";
            case VK_OEM_7: return "Quote";
            default: return "VK:" + std::to_string(vk);
            }
        }

        auto vk_from_label(std::string label, UINT fallback) -> UINT
        {
            label = upper_copy(label);
            if (label.size() == 1 && label[0] >= 'A' && label[0] <= 'Z')
                return static_cast<UINT>(label[0]);
            if (label.size() == 1 && label[0] >= '0' && label[0] <= '9')
                return static_cast<UINT>(label[0]);
            if (label.size() >= 2 && label[0] == 'F')
            {
                const int index = std::atoi(label.c_str() + 1);
                if (index >= 1 && index <= 24)
                    return static_cast<UINT>(VK_F1 + index - 1);
            }
            if (label.rfind("VK:", 0) == 0)
            {
                const int vk = std::atoi(label.c_str() + 3);
                if (vk > 0 && vk < 256)
                    return static_cast<UINT>(vk);
            }
            if (label == "SPACE") return VK_SPACE;
            if (label == "TAB") return VK_TAB;
            if (label == "ENTER") return VK_RETURN;
            if (label == "BACKSPACE") return VK_BACK;
            if (label == "DELETE") return VK_DELETE;
            if (label == "INSERT") return VK_INSERT;
            if (label == "HOME") return VK_HOME;
            if (label == "END") return VK_END;
            if (label == "PAGEUP") return VK_PRIOR;
            if (label == "PAGEDOWN") return VK_NEXT;
            if (label == "LEFT") return VK_LEFT;
            if (label == "RIGHT") return VK_RIGHT;
            if (label == "UP") return VK_UP;
            if (label == "DOWN") return VK_DOWN;
            if (label == "PLUS") return VK_OEM_PLUS;
            if (label == "MINUS") return VK_OEM_MINUS;
            if (label == "COMMA") return VK_OEM_COMMA;
            if (label == "PERIOD") return VK_OEM_PERIOD;
            if (label == "SEMICOLON") return VK_OEM_1;
            if (label == "SLASH") return VK_OEM_2;
            if (label == "BACKQUOTE") return VK_OEM_3;
            if (label == "LEFTBRACKET") return VK_OEM_4;
            if (label == "BACKSLASH") return VK_OEM_5;
            if (label == "RIGHTBRACKET") return VK_OEM_6;
            if (label == "QUOTE") return VK_OEM_7;
            return fallback;
        }

        auto split_tokens(const std::string& text) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::string token;
            for (char c : text)
            {
                if (c == '+')
                {
                    if (!token.empty())
                        out.push_back(token);
                    token.clear();
                    continue;
                }
                if (!std::isspace(static_cast<unsigned char>(c)))
                    token.push_back(c);
            }
            if (!token.empty())
                out.push_back(token);
            return out;
        }

        constexpr int StartHotkeyId = 1;
        constexpr int StopHotkeyId = 2;
        constexpr int PreviewHotkeyId = 3;
        constexpr int UnPreviewHotkeyId = 4;
    }

    auto parse_hotkey_binding(const std::string& text, UINT default_vk) -> HotkeyBinding
    {
        HotkeyBinding binding{};
        binding.vk = is_function_vk(default_vk) ? default_vk : VK_F10;
        binding.modifiers = 0;
        for (const auto& raw : split_tokens(text.empty() ? vk_label(binding.vk) : text))
        {
            const auto token = upper_copy(raw);
            if (token == "CTRL" || token == "CONTROL") binding.modifiers |= MOD_CONTROL;
            else if (token == "ALT") binding.modifiers |= MOD_ALT;
            else if (token == "SHIFT") binding.modifiers |= MOD_SHIFT;
            else if (token == "WIN" || token == "WINDOWS") binding.modifiers |= MOD_WIN;
            else binding.vk = vk_from_label(token, binding.vk);
        }
        if (!is_function_vk(binding.vk))
            binding.vk = is_function_vk(default_vk) ? default_vk : VK_F10;
        binding.modifiers = 0;
        return binding;
    }

    auto hotkey_to_string(const HotkeyBinding& binding) -> std::string
    {
        std::string out;
        if (binding.modifiers & MOD_CONTROL) out += "Ctrl+";
        if (binding.modifiers & MOD_ALT) out += "Alt+";
        if (binding.modifiers & MOD_SHIFT) out += "Shift+";
        if (binding.modifiers & MOD_WIN) out += "Win+";
        out += vk_label(binding.vk);
        return out;
    }

    auto hotkey_backend_json(const HotkeyBinding& start,
                             bool start_registered,
                             const HotkeyBinding& stop,
                             bool stop_registered,
                             const HotkeyBinding& preview,
                             bool preview_registered,
                             const HotkeyBinding& unpreview,
                             bool unpreview_registered) -> std::string
    {
        return std::string("{\"start\":\"") + (start_registered ? "register_hotkey" : "async_state") +
               "\",\"start_key\":\"" + hotkey_to_string(start) +
               "\",\"stop\":\"" + (stop_registered ? "register_hotkey" : "async_state") +
               "\",\"stop_key\":\"" + hotkey_to_string(stop) +
               "\",\"preview\":\"" + (preview_registered ? "register_hotkey" : "async_state") +
               "\",\"preview_key\":\"" + hotkey_to_string(preview) +
               "\",\"unpreview\":\"" + (unpreview_registered ? "register_hotkey" : "async_state") +
               "\",\"unpreview_key\":\"" + hotkey_to_string(unpreview) + "\"}";
    }

    auto try_capture_hotkey_from_message(const MSG& msg, HotkeyBinding& out, std::string& error, bool& cancel) -> bool
    {
        cancel = false;
        if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)
            return false;
        const UINT vk = static_cast<UINT>(msg.wParam);
        if (vk == VK_ESCAPE)
        {
            cancel = true;
            return false;
        }
        if (is_modifier_vk(vk))
        {
            error = "Modifier-only hotkeys are not valid.";
            return false;
        }
        if (!is_function_vk(vk))
        {
            error = "Only function keys F1-F24 are valid hotkeys.";
            return false;
        }
        out.vk = vk;
        out.modifiers = 0;
        error.clear();
        return true;
    }

    OverlayHotkeys::OverlayHotkeys(HotkeyBinding start, HotkeyBinding stop, HotkeyBinding preview, HotkeyBinding unpreview)
        : start_(start), stop_(stop), preview_(preview), unpreview_(unpreview)
    {
        set_hotkeys(start_, stop_, preview_, unpreview_);
    }

    OverlayHotkeys::~OverlayHotkeys()
    {
        unregister_start();
        unregister_stop();
        unregister_preview();
        unregister_unpreview();
    }

    void OverlayHotkeys::unregister_start()
    {
        if (start_registered_)
            UnregisterHotKey(nullptr, StartHotkeyId);
        start_registered_ = false;
    }

    void OverlayHotkeys::unregister_stop()
    {
        if (stop_registered_)
            UnregisterHotKey(nullptr, StopHotkeyId);
        stop_registered_ = false;
    }

    void OverlayHotkeys::unregister_preview()
    {
        if (preview_registered_)
            UnregisterHotKey(nullptr, PreviewHotkeyId);
        preview_registered_ = false;
    }

    void OverlayHotkeys::unregister_unpreview()
    {
        if (unpreview_registered_)
            UnregisterHotKey(nullptr, UnPreviewHotkeyId);
        unpreview_registered_ = false;
    }

    auto OverlayHotkeys::set_start_hotkey(HotkeyBinding start, std::string* error) -> bool
    {
        if (start_registered_ && start.vk == start_.vk && start.modifiers == start_.modifiers)
            return true;

        const HotkeyBinding previous = start_;
        const bool previous_registered = start_registered_;
        unregister_start();
        start_ = start;
        start_down_ = false;
        start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
        if (!start_registered_)
        {
            const DWORD code = GetLastError();
            if (error)
                *error = "RegisterHotKey start failed win32=" + std::to_string(code);
            start_ = previous;
            if (previous_registered)
                start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
            return false;
        }
        if (error)
            error->clear();
        return true;
    }

    auto OverlayHotkeys::set_stop_hotkey(HotkeyBinding stop, std::string* error) -> bool
    {
        if (stop_registered_ && stop.vk == stop_.vk && stop.modifiers == stop_.modifiers)
            return true;

        const HotkeyBinding previous = stop_;
        const bool previous_registered = stop_registered_;
        unregister_stop();
        stop_ = stop;
        stop_down_ = false;
        stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
        if (!stop_registered_)
        {
            const DWORD code = GetLastError();
            if (error)
                *error = "RegisterHotKey stop failed win32=" + std::to_string(code);
            stop_ = previous;
            if (previous_registered)
                stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
            return false;
        }
        if (error)
            error->clear();
        return true;
    }

    auto OverlayHotkeys::set_hotkeys(HotkeyBinding start, HotkeyBinding stop, HotkeyBinding preview, HotkeyBinding unpreview, std::string* error) -> bool
    {
        const HotkeyBinding previous_start = start_;
        const HotkeyBinding previous_stop = stop_;
        const HotkeyBinding previous_preview = preview_;
        const HotkeyBinding previous_unpreview = unpreview_;
        const bool previous_start_registered = start_registered_;
        const bool previous_stop_registered = stop_registered_;
        const bool previous_preview_registered = preview_registered_;
        const bool previous_unpreview_registered = unpreview_registered_;

        unregister_start();
        unregister_stop();
        unregister_preview();
        unregister_unpreview();
        start_ = start;
        stop_ = stop;
        preview_ = preview;
        unpreview_ = unpreview;
        start_down_ = false;
        stop_down_ = false;
        preview_down_ = false;
        unpreview_down_ = false;

        start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
        if (!start_registered_)
        {
            const DWORD code = GetLastError();
            if (error)
                *error = "RegisterHotKey start failed win32=" + std::to_string(code);
            start_ = previous_start;
            stop_ = previous_stop;
            preview_ = previous_preview;
            unpreview_ = previous_unpreview;
            if (previous_start_registered)
                start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
            if (previous_stop_registered)
                stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
            if (previous_preview_registered)
                preview_registered_ = RegisterHotKey(nullptr, PreviewHotkeyId, preview_.modifiers | MOD_NOREPEAT, preview_.vk) != FALSE;
            if (previous_unpreview_registered)
                unpreview_registered_ = RegisterHotKey(nullptr, UnPreviewHotkeyId, unpreview_.modifiers | MOD_NOREPEAT, unpreview_.vk) != FALSE;
            return false;
        }

        stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
        if (!stop_registered_)
        {
            const DWORD code = GetLastError();
            if (error)
                *error = "RegisterHotKey stop failed win32=" + std::to_string(code);
            unregister_start();
            start_ = previous_start;
            stop_ = previous_stop;
            preview_ = previous_preview;
            unpreview_ = previous_unpreview;
            if (previous_start_registered)
                start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
            if (previous_stop_registered)
                stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
            if (previous_preview_registered)
                preview_registered_ = RegisterHotKey(nullptr, PreviewHotkeyId, preview_.modifiers | MOD_NOREPEAT, preview_.vk) != FALSE;
            if (previous_unpreview_registered)
                unpreview_registered_ = RegisterHotKey(nullptr, UnPreviewHotkeyId, unpreview_.modifiers | MOD_NOREPEAT, unpreview_.vk) != FALSE;
            return false;
        }

        preview_registered_ = RegisterHotKey(nullptr, PreviewHotkeyId, preview_.modifiers | MOD_NOREPEAT, preview_.vk) != FALSE;
        if (!preview_registered_)
        {
            const DWORD code = GetLastError();
            if (error)
                *error = "RegisterHotKey preview failed win32=" + std::to_string(code);
            unregister_start();
            unregister_stop();
            start_ = previous_start;
            stop_ = previous_stop;
            preview_ = previous_preview;
            unpreview_ = previous_unpreview;
            if (previous_start_registered)
                start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
            if (previous_stop_registered)
                stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
            if (previous_preview_registered)
                preview_registered_ = RegisterHotKey(nullptr, PreviewHotkeyId, preview_.modifiers | MOD_NOREPEAT, preview_.vk) != FALSE;
            if (previous_unpreview_registered)
                unpreview_registered_ = RegisterHotKey(nullptr, UnPreviewHotkeyId, unpreview_.modifiers | MOD_NOREPEAT, unpreview_.vk) != FALSE;
            return false;
        }

        unpreview_registered_ = RegisterHotKey(nullptr, UnPreviewHotkeyId, unpreview_.modifiers | MOD_NOREPEAT, unpreview_.vk) != FALSE;
        if (!unpreview_registered_)
        {
            const DWORD code = GetLastError();
            if (error)
                *error = "RegisterHotKey unpreview failed win32=" + std::to_string(code);
            unregister_start();
            unregister_stop();
            unregister_preview();
            start_ = previous_start;
            stop_ = previous_stop;
            preview_ = previous_preview;
            unpreview_ = previous_unpreview;
            if (previous_start_registered)
                start_registered_ = RegisterHotKey(nullptr, StartHotkeyId, start_.modifiers | MOD_NOREPEAT, start_.vk) != FALSE;
            if (previous_stop_registered)
                stop_registered_ = RegisterHotKey(nullptr, StopHotkeyId, stop_.modifiers | MOD_NOREPEAT, stop_.vk) != FALSE;
            if (previous_preview_registered)
                preview_registered_ = RegisterHotKey(nullptr, PreviewHotkeyId, preview_.modifiers | MOD_NOREPEAT, preview_.vk) != FALSE;
            if (previous_unpreview_registered)
                unpreview_registered_ = RegisterHotKey(nullptr, UnPreviewHotkeyId, unpreview_.modifiers | MOD_NOREPEAT, unpreview_.vk) != FALSE;
            return false;
        }

        if (error)
            error->clear();
        return true;
    }

    auto OverlayHotkeys::backend_json() const -> std::string
    {
        return hotkey_backend_json(start_, start_registered_, stop_, stop_registered_, preview_, preview_registered_, unpreview_, unpreview_registered_);
    }

    void OverlayHotkeys::handle_message(const MSG& msg, OverlayHotkeyState& state) const
    {
        if (msg.message == WM_HOTKEY && msg.wParam == StartHotkeyId)
            state.start_requested = true;
        if (msg.message == WM_HOTKEY && msg.wParam == StopHotkeyId)
            state.stop_requested = true;
        if (msg.message == WM_HOTKEY && msg.wParam == PreviewHotkeyId)
            state.preview_requested = true;
        if (msg.message == WM_HOTKEY && msg.wParam == UnPreviewHotkeyId)
            state.unpreview_requested = true;
    }

    void OverlayHotkeys::poll_fallback(OverlayHotkeyState& state)
    {
        if (!start_registered_)
        {
            const bool down = (GetAsyncKeyState(static_cast<int>(start_.vk)) & 0x8000) != 0;
            state.start_requested = state.start_requested || (down && !start_down_);
            start_down_ = down;
        }
        if (!stop_registered_)
        {
            const bool down = (GetAsyncKeyState(static_cast<int>(stop_.vk)) & 0x8000) != 0;
            state.stop_requested = state.stop_requested || (down && !stop_down_);
            stop_down_ = down;
        }
        if (!preview_registered_)
        {
            const bool down = (GetAsyncKeyState(static_cast<int>(preview_.vk)) & 0x8000) != 0;
            state.preview_requested = state.preview_requested || (down && !preview_down_);
            preview_down_ = down;
        }
        if (!unpreview_registered_)
        {
            const bool down = (GetAsyncKeyState(static_cast<int>(unpreview_.vk)) & 0x8000) != 0;
            state.unpreview_requested = state.unpreview_requested || (down && !unpreview_down_);
            unpreview_down_ = down;
        }
    }
}
