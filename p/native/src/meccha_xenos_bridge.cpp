#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../sdk/meccha_sdk_min.hpp"

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    constexpr int DefaultBridgePort = 47654;
    constexpr int IdleShutdownSeconds = 15;
    constexpr std::size_t MaxRequestBytes = 8 * 1024 * 1024;
    constexpr int PaintChannelAlbedoMetallicRoughness = 5;
    constexpr int ProcessEventVtableIndex = meccha_sdk::Offsets::ProcessEventIdx;
    constexpr UINT PaintDispatchMessage = WM_APP + 0x4D43;

    constexpr std::uintptr_t OffClass = 0x10;
    constexpr std::uintptr_t OffName = 0x18;
    constexpr std::uintptr_t OffOuter = 0x20;
    constexpr std::uintptr_t OffObjectFlags = 0x08;
    constexpr std::uint32_t RFClassDefaultObject = 0x10;
    constexpr std::uintptr_t OffSuperStruct = 0x40;
    constexpr std::uintptr_t OffChildren = 0x48;
    constexpr std::uintptr_t OffChildProperties = 0x50;
    constexpr std::uintptr_t OffPropertiesSize = 0x58;
    constexpr std::uintptr_t OffUFieldNext = 0x28;
    constexpr std::uintptr_t OffFFieldNext = 0x18;
    constexpr std::uintptr_t OffFFieldName = 0x20;
    constexpr std::uintptr_t OffFPropertyElementSize = 0x3C;
    constexpr std::uintptr_t OffFPropertyOffset = 0x44;
    constexpr std::uintptr_t OffFStructPropertyStruct = 0x78;

    HMODULE g_module = nullptr;
    std::atomic<bool> g_running{true};
    std::atomic<bool> g_process_event_hook_installed{false};
    std::atomic<std::uintptr_t> g_original_process_event{0};
    std::atomic<HHOOK> g_message_hook{nullptr};
    std::atomic<DWORD> g_game_thread_id{0};
    std::mutex g_hook_mutex;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> g_process_event_hook_slots;
    thread_local bool g_inside_process_event_hook = false;

    using ProcessEventFn = void(__fastcall*)(void*, void*, void*);

    struct QueuedPaintJob
    {
        std::string request{};
        std::string response{};
        bool done{false};
    };

    std::mutex g_paint_jobs_mutex;
    std::condition_variable g_paint_jobs_cv;
    std::vector<std::shared_ptr<QueuedPaintJob>> g_paint_jobs;

    auto paint_full_route_native_direct(const std::string& request) -> std::string;
    auto drain_paint_jobs_on_game_thread() -> void;
    void __fastcall hooked_process_event(void* object, void* function, void* params);
    LRESULT CALLBACK message_hook_proc(int code, WPARAM wparam, LPARAM lparam);

    template <typename T>
    auto safe_read(std::uintptr_t address, T fallback = T{}) -> T
    {
        __try
        {
            return *reinterpret_cast<T*>(address);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return fallback;
        }
    }

    auto safe_copy(void* dest, const void* src, std::size_t size) -> bool
    {
        __try
        {
            std::memcpy(dest, src, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto lower_copy(std::string text) -> std::string
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    auto contains_text(const std::string& text, const char* needle) -> bool
    {
        return text.find(needle) != std::string::npos;
    }

    auto json_escape(const std::string& text) -> std::string
    {
        std::string out{};
        out.reserve(text.size() + 8);
        for (const char c : text)
        {
            if (c == '\\' || c == '"')
            {
                out.push_back('\\');
                out.push_back(c);
            }
            else if (c == '\n')
            {
                out += "\\n";
            }
            else if (c == '\r')
            {
                out += "\\r";
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    auto response_json(bool success,
                       const char* stage,
                       int applied,
                       int failures,
                       const std::string& message,
                       const std::string& metadata = "") -> std::string
    {
        std::string out = "{\"success\":";
        out += success ? "true" : "false";
        out += ",\"stage\":\"";
        out += stage;
        out += "\",\"applied\":";
        out += std::to_string(applied);
        out += ",\"failures\":";
        out += std::to_string(failures);
        out += ",\"message\":\"";
        out += json_escape(message);
        out += "\",\"timing_ms\":{},\"metadata\":{\"bridge\":\"meccha-xenos-bridge\"";
        if (!metadata.empty())
        {
            out += ",";
            out += metadata;
        }
        out += "}}\n";
        return out;
    }

    auto resolve_bridge_port() -> int
    {
        wchar_t dll_path[MAX_PATH]{};
        if (g_module != nullptr && GetModuleFileNameW(g_module, dll_path, MAX_PATH) > 0)
        {
            std::wstring port_path = dll_path;
            port_path += L".port";
            HANDLE file = CreateFileW(port_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                char buffer[32]{};
                DWORD bytes_read = 0;
                const BOOL ok = ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &bytes_read, nullptr);
                CloseHandle(file);
                if (ok && bytes_read > 0)
                {
                    const int port = std::atoi(buffer);
                    if (port >= 1024 && port <= 65535)
                    {
                        return port;
                    }
                }
            }
        }
        return DefaultBridgePort;
    }

    struct ModuleRange
    {
        std::uintptr_t base{0};
        std::size_t size{0};
    };

    auto main_module_range() -> ModuleRange
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base)
        {
            return {};
        }
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return {};
        }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return {};
        }
        return {reinterpret_cast<std::uintptr_t>(base), nt->OptionalHeader.SizeOfImage};
    }

    auto address_in_main_module(std::uintptr_t address) -> bool
    {
        const auto module = main_module_range();
        return module.base && address >= module.base && address < module.base + module.size;
    }

    auto live_uobject(std::uintptr_t object) -> bool
    {
        if (!object || address_in_main_module(object))
        {
            return false;
        }
        const auto flags = safe_read<std::uint32_t>(object + OffObjectFlags, 0);
        return (flags & RFClassDefaultObject) == 0;
    }

    auto match_pattern(const std::uint8_t* data, const std::uint8_t* pattern, const std::uint8_t* mask, std::size_t length) -> bool
    {
        for (std::size_t i = 0; i < length; ++i)
        {
            if (mask[i] && data[i] != pattern[i])
            {
                return false;
            }
        }
        return true;
    }

    auto scan_pattern(const std::vector<std::uint8_t>& pattern, const std::vector<std::uint8_t>& mask) -> std::uintptr_t
    {
        const auto module = main_module_range();
        if (!module.base || !module.size || pattern.empty() || pattern.size() != mask.size())
        {
            return 0;
        }
        const auto* base = reinterpret_cast<const std::uint8_t*>(module.base);
        const std::size_t length = pattern.size();
        for (std::size_t offset = 0; offset + length < module.size; ++offset)
        {
            __try
            {
                if (match_pattern(base + offset, pattern.data(), mask.data(), length))
                {
                    return module.base + offset;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return 0;
    }

    struct FNameResolver
    {
        std::uintptr_t pool{0};
        int table_offset{0x10};
        int style{1};
        const int offsets[14]{0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70};

        auto entry(std::uint32_t id, int table, int entry_style) const -> std::string
        {
            const auto block_index = id >> 16;
            const auto within = (id & 0xFFFF) << 1;
            const auto block = safe_read<std::uintptr_t>(pool + table + static_cast<std::uintptr_t>(block_index) * 8);
            if (!block)
            {
                return {};
            }
            const auto header = safe_read<std::uint16_t>(block + within);
            bool wide = false;
            int length = 0;
            if (entry_style == 0)
            {
                wide = (header & 1) != 0;
                length = header >> 1;
            }
            else if (entry_style == 2)
            {
                wide = (header & 1) != 0;
                length = (header >> 6) & 0x3FF;
            }
            else
            {
                length = header & 0x3FF;
                wide = ((header >> 10) & 1) != 0;
            }
            if (length <= 0 || length > 512)
            {
                return {};
            }
            if (wide)
            {
                std::wstring text(length, L'\0');
                if (!safe_copy(text.data(), reinterpret_cast<void*>(block + within + 2), static_cast<std::size_t>(length) * sizeof(wchar_t)))
                {
                    return {};
                }
                std::string out{};
                out.reserve(text.size());
                for (wchar_t c : text)
                {
                    out.push_back(c >= 0 && c < 128 ? static_cast<char>(c) : '?');
                }
                return out;
            }
            std::string text(length, '\0');
            if (!safe_copy(text.data(), reinterpret_cast<void*>(block + within + 2), static_cast<std::size_t>(length)))
            {
                return {};
            }
            return text;
        }

        auto detect() -> void
        {
            for (const int off : offsets)
            {
                for (const int st : {2, 1, 0})
                {
                    if (entry(0, off, st) == "None")
                    {
                        table_offset = off;
                        style = st;
                        return;
                    }
                }
            }
        }

        auto resolve(std::uint32_t id) -> std::string
        {
            auto name = entry(id, table_offset, style);
            if (!name.empty())
            {
                return name;
            }
            for (const int off : offsets)
            {
                for (const int st : {2, 1, 0})
                {
                    name = entry(id, off, st);
                    if (!name.empty())
                    {
                        table_offset = off;
                        style = st;
                        return name;
                    }
                }
            }
            return {};
        }
    };

    struct Reflection
    {
        std::uintptr_t guobject_array{0};
        std::uintptr_t fname_pool{0};
        FNameResolver names{};
        std::uintptr_t meta_class{0};

        auto init(std::string& failure) -> bool
        {
            static const std::vector<std::uint8_t> gu_sig{0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x48, 0x89, 0x01, 0x45, 0x8B, 0xD1};
            static const std::vector<std::uint8_t> gu_mask{1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
            const auto gu_ref = scan_pattern(gu_sig, gu_mask);
            if (!gu_ref)
            {
                failure = "guobject_pattern_not_found";
                return false;
            }
            const auto rel = safe_read<std::int32_t>(gu_ref + 3);
            guobject_array = gu_ref + 7 + rel;
            const auto delta_candidate = guobject_array - 0xE3B40;
            names.pool = delta_candidate;
            names.detect();
            if (names.resolve(0) == "None")
            {
                fname_pool = delta_candidate;
                return true;
            }

            const std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> patterns{
                {{0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x4C, 0x8B, 0xC0}, {1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1}},
                {{0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x48, 0x8B}, {1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1}},
                {{0x48, 0x8D, 0x35, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 0, 0}},
                {{0x48, 0x8D, 0x3D, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 0, 0}},
            };
            for (const auto& [sig, mask] : patterns)
            {
                const auto ref = scan_pattern(sig, mask);
                if (!ref)
                {
                    continue;
                }
                const auto fname_rel = safe_read<std::int32_t>(ref + 3);
                names.pool = ref + 7 + fname_rel;
                names.detect();
                if (names.resolve(0) == "None")
                {
                    fname_pool = names.pool;
                    return true;
                }
            }
            failure = "fname_pool_not_found";
            return false;
        }

        auto object_name(std::uintptr_t object) -> std::string
        {
            if (!object)
            {
                return {};
            }
            auto out = names.resolve(safe_read<std::uint32_t>(object + OffName));
            const auto slash = out.find_last_of("/.");
            if (slash != std::string::npos)
            {
                out = out.substr(slash + 1);
            }
            if (out.rfind("Default__", 0) == 0)
            {
                out = out.substr(9);
            }
            return out;
        }

        auto class_ptr(std::uintptr_t object) -> std::uintptr_t
        {
            return object ? safe_read<std::uintptr_t>(object + OffClass) : 0;
        }

        auto class_name(std::uintptr_t object) -> std::string
        {
            return object_name(class_ptr(object));
        }

        template <typename Fn>
        auto for_each_object(Fn fn) -> void
        {
            const auto chunks = safe_read<std::uintptr_t>(guobject_array + 0x10);
            if (!chunks)
            {
                return;
            }
            for (int ci = 0; ci < 64; ++ci)
            {
                const auto chunk = safe_read<std::uintptr_t>(chunks + static_cast<std::uintptr_t>(ci) * 8);
                if (!chunk)
                {
                    break;
                }
                for (int wi = 0; wi < 65536; ++wi)
                {
                    const auto obj = safe_read<std::uintptr_t>(chunk + static_cast<std::uintptr_t>(wi) * 0x18);
                    if (obj && fn(obj))
                    {
                        return;
                    }
                }
            }
        }

        auto find_meta_class() -> std::uintptr_t
        {
            if (meta_class)
            {
                return meta_class;
            }
            for_each_object([&](std::uintptr_t obj) {
                if (object_name(obj) == "Class")
                {
                    meta_class = obj;
                    return true;
                }
                return false;
            });
            return meta_class;
        }

        auto find_class(const char* name) -> std::uintptr_t
        {
            const auto meta = find_meta_class();
            if (!meta)
            {
                return 0;
            }
            std::uintptr_t found = 0;
            for_each_object([&](std::uintptr_t obj) {
                if (class_ptr(obj) == meta && object_name(obj) == name)
                {
                    found = obj;
                    return true;
                }
                return false;
            });
            return found;
        }

        auto find_first_instance(const char* class_name_text) -> std::uintptr_t
        {
            const auto cls = find_class(class_name_text);
            if (!cls)
            {
                return 0;
            }
            std::uintptr_t found = 0;
            for_each_object([&](std::uintptr_t obj) {
                if (class_ptr(obj) == cls && object_name(obj).rfind("Default__", 0) != 0)
                {
                    found = obj;
                    return true;
                }
                return false;
            });
            return found;
        }

        auto find_property(std::uintptr_t structure, const char* property_name) -> std::uintptr_t
        {
            for (auto prop = safe_read<std::uintptr_t>(structure + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
            {
                if (names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)) == property_name)
                {
                    return prop;
                }
            }
            return 0;
        }

        auto resolve_property_offset(const char* class_name_text, const char* property_name) -> int
        {
            auto cls = find_class(class_name_text);
            for (int depth = 0; cls && depth < 32; ++depth)
            {
                const auto prop = find_property(cls, property_name);
                if (prop)
                {
                    return safe_read<int>(prop + OffFPropertyOffset, -1);
                }
                cls = safe_read<std::uintptr_t>(cls + OffSuperStruct);
            }
            return -1;
        }

        auto find_function(std::uintptr_t object, const char* function_name) -> std::uintptr_t
        {
            auto cls = class_ptr(object);
            for (int depth = 0; cls && depth < 64; ++depth)
            {
                for (auto child = safe_read<std::uintptr_t>(cls + OffChildren); child; child = safe_read<std::uintptr_t>(child + OffUFieldNext))
                {
                    if (object_name(child) == function_name)
                    {
                        return child;
                    }
                }
                cls = safe_read<std::uintptr_t>(cls + OffSuperStruct);
            }
            return 0;
        }
    };

    struct Color
    {
        double r{1.0};
        double g{1.0};
        double b{1.0};
        double roughness{0.0};
        double metallic{1.0};
        int apply_mode{0};
    };

    struct PaintCallStats
    {
        int server_success{0};
        int server_failure{0};
        int local_success{0};
        int local_failure{0};
        std::string first_failure{};
    };

    struct ScriptArrayParam
    {
        void* data{nullptr};
        int num{0};
        int max{0};
    };

    struct ChannelBuffer
    {
        bool ok{false};
        int channel{0};
        int width{0};
        int height{0};
        int bytes_per_pixel{1};
        std::uint64_t hash{1469598103934665603ULL};
        std::string failure{};
        std::vector<std::uint8_t> bytes{};
    };

    struct FrontSample
    {
        double u{0.5};
        double v{0.5};
        double r{1.0};
        double g{1.0};
        double b{1.0};
        double roughness{0.65};
        double metallic{0.0};
        double radius{0.012};
        bool floor_like{false};
        int atlas_priority{0};
        int atlas_radius{2};
        double atlas_weight{0.0};
        double screen_nx{0.5};
        double screen_ny{0.5};
        bool has_world_position{false};
        meccha_sdk::FVector world_position{};
        meccha_sdk::FVector normal{};
    };

    auto clamp01(double value) -> double;
    auto prop_offset(std::uintptr_t prop) -> int;
    auto prop_element_size(std::uintptr_t prop) -> int;
    auto write_number(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, double value) -> bool;
    auto process_event(std::uintptr_t object, std::uintptr_t function, std::uint8_t* params, std::string& failure) -> bool;
    auto read_return_bool(Reflection& ref, std::uintptr_t function, std::uint8_t* params) -> bool;

    auto fnv1a_update(std::uint64_t hash, const void* data, std::size_t size) -> std::uint64_t
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<std::uint64_t>(bytes[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    auto hash_bytes(const std::vector<std::uint8_t>& bytes) -> std::uint64_t
    {
        return bytes.empty() ? 1469598103934665603ULL : fnv1a_update(1469598103934665603ULL, bytes.data(), bytes.size());
    }

    auto infer_channel_dimensions(std::size_t byte_count) -> std::pair<int, int>
    {
        if (byte_count == 0)
        {
            return {0, 0};
        }
        const auto pixels_rgba = byte_count % 4 == 0 ? byte_count / 4 : byte_count;
        int width = static_cast<int>(std::sqrt(static_cast<double>(pixels_rgba)));
        width = std::max(1, width);
        while (width > 1 && pixels_rgba % static_cast<std::size_t>(width) != 0)
        {
            --width;
        }
        const int height = static_cast<int>(pixels_rgba / static_cast<std::size_t>(width));
        return {width, std::max(1, height)};
    }

    auto find_array_param(Reflection& ref, std::uintptr_t function) -> std::uintptr_t
    {
        std::uintptr_t fallback = 0;
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue" || contains_text(name, "channel"))
            {
                continue;
            }
            const auto offset = prop_offset(prop);
            if (offset >= 0 && params_size > 0 && offset + static_cast<int>(sizeof(ScriptArrayParam)) <= params_size)
            {
                if (!fallback)
                {
                    fallback = prop;
                }
                if (contains_text(name, "byte") || contains_text(name, "data") || contains_text(name, "buffer") ||
                    contains_text(name, "array") || contains_text(name, "out"))
                {
                    return prop;
                }
            }
        }
        return fallback;
    }

    auto write_channel_param(Reflection& ref, std::uintptr_t function, std::uint8_t* params, int channel) -> bool
    {
        bool wrote = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (contains_text(name, "channel"))
            {
                wrote = write_number(ref, prop, params, static_cast<double>(channel)) || wrote;
            }
        }
        return wrote;
    }

    auto export_channel_bytes(Reflection& ref, std::uintptr_t component, int channel) -> ChannelBuffer
    {
        ChannelBuffer out{};
        out.channel = channel;
        const auto function = ref.find_function(component, "ExportChannelToBytes");
        if (!function)
        {
            out.failure = "export_unavailable";
            return out;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 8192)
        {
            out.failure = "export_params_size_invalid";
            return out;
        }
        const auto array_prop = find_array_param(ref, function);
        if (!array_prop)
        {
            out.failure = "export_array_param_unavailable";
            return out;
        }
        const auto array_offset = prop_offset(array_prop);
        if (array_offset < 0)
        {
            out.failure = "export_array_offset_invalid";
            return out;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        write_channel_param(ref, function, params.data(), channel);
        std::string failure{};
        if (!process_event(component, function, params.data(), failure))
        {
            out.failure = "export_process_event_failed:" + failure;
            return out;
        }
        const bool return_ok = read_return_bool(ref, function, params.data());
        const auto array = safe_read<ScriptArrayParam>(reinterpret_cast<std::uintptr_t>(params.data() + array_offset));
        if (!array.data || array.num <= 0 || array.num > 128 * 1024 * 1024)
        {
            out.failure = return_ok ? "export_array_invalid" : "export_return_false_array_invalid";
            return out;
        }
        out.bytes.resize(static_cast<std::size_t>(array.num));
        if (!safe_copy(out.bytes.data(), array.data, out.bytes.size()))
        {
            out.failure = "export_array_copy_failed";
            out.bytes.clear();
            return out;
        }
        out.hash = hash_bytes(out.bytes);
        const auto [width, height] = infer_channel_dimensions(out.bytes.size());
        out.width = width;
        out.height = height;
        out.bytes_per_pixel = (width > 0 && height > 0 && out.bytes.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) ? 4 : 1;
        out.ok = true;
        if (!return_ok)
        {
            out.failure = "export_return_false_but_array_observed";
        }
        return out;
    }

    auto import_channel_bytes(Reflection& ref, std::uintptr_t component, int channel, const std::vector<std::uint8_t>& bytes, std::string& failure) -> bool
    {
        failure.clear();
        const auto function = ref.find_function(component, "ImportChannelFromBytes");
        if (!function)
        {
            failure = "import_unavailable";
            return false;
        }
        if (bytes.empty() || bytes.size() > 128 * 1024 * 1024)
        {
            failure = "import_bytes_invalid";
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 8192)
        {
            failure = "import_params_size_invalid";
            return false;
        }
        const auto array_prop = find_array_param(ref, function);
        if (!array_prop)
        {
            failure = "import_array_param_unavailable";
            return false;
        }
        const auto array_offset = prop_offset(array_prop);
        if (array_offset < 0)
        {
            failure = "import_array_offset_invalid";
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        write_channel_param(ref, function, params.data(), channel);
        auto* array = reinterpret_cast<ScriptArrayParam*>(params.data() + array_offset);
        array->data = const_cast<std::uint8_t*>(bytes.data());
        array->num = static_cast<int>(bytes.size());
        array->max = static_cast<int>(bytes.size());
        if (!process_event(component, function, params.data(), failure))
        {
            failure = "import_process_event_failed:" + failure;
            return false;
        }
        if (!read_return_bool(ref, function, params.data()))
        {
            failure = "import_return_false_observation_required";
        }
        return true;
    }

    auto parse_front_samples(const std::string& request, int limit = 4096) -> std::vector<FrontSample>
    {
        std::vector<FrontSample> samples{};
        std::size_t pos = request.find("\"front_samples\"");
        if (pos == std::string::npos)
        {
            return samples;
        }
        while (static_cast<int>(samples.size()) < limit)
        {
            const auto up = request.find("\"u\":", pos);
            if (up == std::string::npos)
            {
                break;
            }
            const auto vp = request.find("\"v\":", up);
            const auto rp = request.find("\"r\":", vp == std::string::npos ? up : vp);
            const auto gp = request.find("\"g\":", rp == std::string::npos ? up : rp);
            const auto bp = request.find("\"b\":", gp == std::string::npos ? up : gp);
            if (vp == std::string::npos || rp == std::string::npos || gp == std::string::npos || bp == std::string::npos)
            {
                break;
            }
            const auto sample_end = request.find('}', bp);
            char* end = nullptr;
            FrontSample sample{};
            sample.u = clamp01(std::strtod(request.c_str() + up + 4, &end));
            sample.v = clamp01(std::strtod(request.c_str() + vp + 4, &end));
            sample.r = clamp01(std::strtod(request.c_str() + rp + 4, &end));
            sample.g = clamp01(std::strtod(request.c_str() + gp + 4, &end));
            sample.b = clamp01(std::strtod(request.c_str() + bp + 4, &end));
            const auto roughp = request.find("\"roughness\":", bp);
            if (roughp != std::string::npos && sample_end != std::string::npos && roughp < sample_end)
            {
                sample.roughness = clamp01(std::strtod(request.c_str() + roughp + 12, &end));
            }
            const auto radiusp = request.find("\"radius\":", bp);
            if (radiusp != std::string::npos && sample_end != std::string::npos && radiusp < sample_end)
            {
                sample.radius = std::min(0.08, std::max(0.001, std::strtod(request.c_str() + radiusp + 9, &end)));
            }
            const auto wxp = request.find("\"world_x\":", bp);
            const auto wyp = request.find("\"world_y\":", bp);
            const auto wzp = request.find("\"world_z\":", bp);
            if (sample_end != std::string::npos &&
                wxp != std::string::npos && wyp != std::string::npos && wzp != std::string::npos &&
                wxp < sample_end && wyp < sample_end && wzp < sample_end)
            {
                sample.world_position.X = std::strtod(request.c_str() + wxp + 10, &end);
                sample.world_position.Y = std::strtod(request.c_str() + wyp + 10, &end);
                sample.world_position.Z = std::strtod(request.c_str() + wzp + 10, &end);
                sample.has_world_position = true;
            }
            samples.push_back(sample);
            pos = bp + 4;
        }
        return samples;
    }

    auto fill_channel(std::vector<std::uint8_t>& bytes, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) -> void
    {
        if (bytes.empty())
        {
            return;
        }
        if (bytes.size() % 4 == 0)
        {
            for (std::size_t i = 0; i + 3 < bytes.size(); i += 4)
            {
                bytes[i + 0] = r;
                bytes[i + 1] = g;
                bytes[i + 2] = b;
                bytes[i + 3] = a;
            }
            return;
        }
        std::fill(bytes.begin(), bytes.end(), r);
    }

    auto paint_disc(std::vector<std::uint8_t>& bytes, int width, int height, const FrontSample& sample, bool albedo) -> void
    {
        if (bytes.empty() || width <= 0 || height <= 0)
        {
            return;
        }
        const bool rgba = bytes.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        const int cx = std::min(width - 1, std::max(0, static_cast<int>(sample.u * static_cast<double>(width))));
        const int cy = std::min(height - 1, std::max(0, static_cast<int>((1.0 - sample.v) * static_cast<double>(height))));
        const int radius = std::max(1, static_cast<int>(sample.radius * static_cast<double>(std::min(width, height))));
        const int r2 = radius * radius;
        const auto rb = static_cast<std::uint8_t>(std::round(clamp01(sample.r) * 255.0));
        const auto gb = static_cast<std::uint8_t>(std::round(clamp01(sample.g) * 255.0));
        const auto bb = static_cast<std::uint8_t>(std::round(clamp01(sample.b) * 255.0));
        const auto scalar = static_cast<std::uint8_t>(std::round(clamp01(sample.roughness) * 255.0));
        for (int y = std::max(0, cy - radius); y <= std::min(height - 1, cy + radius); ++y)
        {
            for (int x = std::max(0, cx - radius); x <= std::min(width - 1, cx + radius); ++x)
            {
                const int dx = x - cx;
                const int dy = y - cy;
                if ((dx * dx + dy * dy) > r2)
                {
                    continue;
                }
                const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                if (rgba)
                {
                    const auto offset = pixel * 4;
                    bytes[offset + 0] = albedo ? rb : scalar;
                    bytes[offset + 1] = albedo ? gb : scalar;
                    bytes[offset + 2] = albedo ? bb : scalar;
                    bytes[offset + 3] = 255;
                }
                else if (pixel < bytes.size())
                {
                    bytes[pixel] = albedo ? rb : scalar;
                }
            }
        }
    }

    auto normalize_material_channel_layout(ChannelBuffer& channel, const ChannelBuffer& albedo) -> void
    {
        if (!channel.ok || !albedo.ok || albedo.width <= 0 || albedo.height <= 0)
        {
            return;
        }
        const auto pixels = static_cast<std::size_t>(albedo.width) * static_cast<std::size_t>(albedo.height);
        if (pixels == 0)
        {
            return;
        }
        if (channel.bytes.size() == pixels)
        {
            channel.width = albedo.width;
            channel.height = albedo.height;
            channel.bytes_per_pixel = 1;
        }
        else if (channel.bytes.size() == pixels * 4)
        {
            channel.width = albedo.width;
            channel.height = albedo.height;
            channel.bytes_per_pixel = 4;
        }
    }

    auto fill_material_channel(std::vector<std::uint8_t>& bytes, int bytes_per_pixel, std::uint8_t value) -> void
    {
        if (bytes.empty())
        {
            return;
        }
        if (bytes_per_pixel >= 4)
        {
            for (std::size_t i = 0; i + 3 < bytes.size(); i += 4)
            {
                bytes[i + 0] = value;
                bytes[i + 1] = value;
                bytes[i + 2] = value;
                bytes[i + 3] = 255;
            }
            return;
        }
        std::fill(bytes.begin(), bytes.end(), value);
    }

    auto paint_material_disc(std::vector<std::uint8_t>& bytes,
                             int width,
                             int height,
                             int bytes_per_pixel,
                             const FrontSample& sample,
                             std::uint8_t value) -> void
    {
        if (bytes.empty() || width <= 0 || height <= 0)
        {
            return;
        }
        bytes_per_pixel = bytes_per_pixel >= 4 ? 4 : 1;
        const int cx = std::min(width - 1, std::max(0, static_cast<int>(sample.u * static_cast<double>(width))));
        const int cy = std::min(height - 1, std::max(0, static_cast<int>((1.0 - sample.v) * static_cast<double>(height))));
        const int radius = std::max(1, static_cast<int>(sample.radius * static_cast<double>(std::min(width, height))));
        const int r2 = radius * radius;
        for (int y = std::max(0, cy - radius); y <= std::min(height - 1, cy + radius); ++y)
        {
            for (int x = std::max(0, cx - radius); x <= std::min(width - 1, cx + radius); ++x)
            {
                const int dx = x - cx;
                const int dy = y - cy;
                if ((dx * dx + dy * dy) > r2)
                {
                    continue;
                }
                const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                const auto offset = pixel * static_cast<std::size_t>(bytes_per_pixel);
                if (offset >= bytes.size())
                {
                    continue;
                }
                if (bytes_per_pixel >= 4 && offset + 3 < bytes.size())
                {
                    bytes[offset + 0] = value;
                    bytes[offset + 1] = value;
                    bytes[offset + 2] = value;
                    bytes[offset + 3] = 255;
                }
                else
                {
                    bytes[offset] = value;
                }
            }
        }
    }

    auto clamp01(double value) -> double
    {
        return std::max(0.0, std::min(1.0, value));
    }

    auto prop_offset(std::uintptr_t prop) -> int
    {
        return safe_read<int>(prop + OffFPropertyOffset, -1);
    }

    auto prop_element_size(std::uintptr_t prop) -> int
    {
        return safe_read<int>(prop + OffFPropertyElementSize, 0);
    }

    auto find_property_any(Reflection& ref, std::uintptr_t structure, std::initializer_list<const char*> field_names) -> std::uintptr_t
    {
        if (!structure)
        {
            return 0;
        }
        for (const auto* field_name : field_names)
        {
            if (field_name)
            {
                if (const auto prop = ref.find_property(structure, field_name))
                {
                    return prop;
                }
            }
        }
        for (auto prop = safe_read<std::uintptr_t>(structure + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto prop_name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            for (const auto* field_name : field_names)
            {
                if (field_name && prop_name == lower_copy(field_name))
                {
                    return prop;
                }
            }
        }
        return 0;
    }

    auto struct_has_any_field(Reflection& ref, std::uintptr_t structure, std::initializer_list<const char*> field_names) -> bool
    {
        return find_property_any(ref, structure, field_names) != 0;
    }

    auto struct_type(Reflection& ref, std::uintptr_t prop, std::initializer_list<const char*> field_names) -> std::uintptr_t
    {
        const std::uintptr_t candidate_offsets[]{
            OffFStructPropertyStruct,
            0x68,
            0x70,
            0x80,
            0x88,
            0x90,
            0x98,
            0xA0,
        };
        for (const auto offset : candidate_offsets)
        {
            const auto structure = safe_read<std::uintptr_t>(prop + offset);
            if (struct_has_any_field(ref, structure, field_names))
            {
                return structure;
            }
        }
        return safe_read<std::uintptr_t>(prop + OffFStructPropertyStruct);
    }

    auto strict_vector_struct_type(Reflection& ref, std::uintptr_t prop, std::initializer_list<const char*> field_names, int min_size) -> std::uintptr_t
    {
        if (prop_element_size(prop) < min_size)
        {
            return 0;
        }
        const auto structure = struct_type(ref, prop, field_names);
        if (!structure)
        {
            return 0;
        }
        for (const auto* field_name : field_names)
        {
            if (!field_name || !find_property_any(ref, structure, {field_name}))
            {
                return 0;
            }
        }
        return structure;
    }

    auto function_param_schema(Reflection& ref, std::uintptr_t function) -> std::string
    {
        std::string out{};
        int count = 0;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop && count < 32; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext), ++count)
        {
            if (!out.empty())
            {
                out += ";";
            }
            const auto name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            out += name + "@" + std::to_string(prop_offset(prop)) + "#" + std::to_string(prop_element_size(prop));
        }
        return out;
    }

    auto find_object_property(Reflection& ref, std::uintptr_t object, const char* property_name) -> std::uintptr_t
    {
        auto cls = ref.class_ptr(object);
        for (int depth = 0; cls && depth < 32; ++depth)
        {
            const auto prop = ref.find_property(cls, property_name);
            if (prop)
            {
                return prop;
            }
            cls = safe_read<std::uintptr_t>(cls + OffSuperStruct);
        }
        return 0;
    }

    auto write_number(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, double value) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* dest = container + offset;
        const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
        const auto size = prop_element_size(prop);
        const bool integral = contains_text(name, "channel") || contains_text(name, "mode") || contains_text(name, "level") ||
                              contains_text(name, "resolution") || contains_text(name, "triangles") || contains_text(name, "pixels");
        if (integral)
        {
            if (size <= 1)
            {
                *dest = static_cast<std::uint8_t>(value);
            }
            else
            {
                *reinterpret_cast<std::int32_t*>(dest) = static_cast<std::int32_t>(value);
            }
            return true;
        }
        if (size == 8)
        {
            *reinterpret_cast<double*>(dest) = value;
            return true;
        }
        if (size >= 4)
        {
            *reinterpret_cast<float*>(dest) = static_cast<float>(value);
            return true;
        }
        if (size == 1)
        {
            *dest = static_cast<std::uint8_t>(value);
            return true;
        }
        return false;
    }

    auto write_bool(std::uintptr_t prop, std::uint8_t* container, bool value) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        *(container + offset) = value ? 1 : 0;
        return true;
    }

    auto write_vector2(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, double u, double v) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* dest = container + offset;
        const auto st = struct_type(ref, prop, {"X", "Y"});
        bool wrote = false;
        if (st)
        {
            if (const auto x = find_property_any(ref, st, {"X"}))
            {
                wrote = write_number(ref, x, dest, u) || wrote;
            }
            if (const auto y = find_property_any(ref, st, {"Y"}))
            {
                wrote = write_number(ref, y, dest, v) || wrote;
            }
            if (wrote)
            {
                return true;
            }
        }
        const auto size = prop_element_size(prop);
        if (size >= 16)
        {
            const double values[2]{u, v};
            std::memcpy(dest, values, sizeof(values));
            return true;
        }
        if (size >= 8)
        {
            const float values[2]{static_cast<float>(u), static_cast<float>(v)};
            std::memcpy(dest, values, sizeof(values));
            return true;
        }
        return false;
    }

    auto write_linear_color(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, const Color& color) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* dest = container + offset;
        const auto st = struct_type(ref, prop, {"R", "G", "B", "A"});
        bool wrote = false;
        if (st)
        {
            if (const auto p = find_property_any(ref, st, {"R"})) wrote = write_number(ref, p, dest, color.r) || wrote;
            if (const auto p = find_property_any(ref, st, {"G"})) wrote = write_number(ref, p, dest, color.g) || wrote;
            if (const auto p = find_property_any(ref, st, {"B"})) wrote = write_number(ref, p, dest, color.b) || wrote;
            if (const auto p = find_property_any(ref, st, {"A"})) wrote = write_number(ref, p, dest, 1.0) || wrote;
            if (wrote) return true;
        }
        const float values[4]{static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), 1.0f};
        std::memcpy(dest, values, std::min<std::size_t>(16, static_cast<std::size_t>(std::max(0, prop_element_size(prop)))));
        return true;
    }

    auto write_channel_data(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, const Color& color, bool include_material) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        const auto st = struct_type(ref, prop, {"AlbedoColor", "Metallic", "Roughness", "Height", "ApplyMode"});
        if (!st)
        {
            return false;
        }
        bool wrote = false;
        if (const auto p = find_property_any(ref, st, {"AlbedoColor"})) wrote = write_linear_color(ref, p, base, color) || wrote;
        if (include_material)
        {
            if (const auto p = find_property_any(ref, st, {"Metallic"})) wrote = write_number(ref, p, base, clamp01(color.metallic)) || wrote;
            if (const auto p = find_property_any(ref, st, {"Roughness"})) wrote = write_number(ref, p, base, clamp01(color.roughness)) || wrote;
        }
        if (const auto p = find_property_any(ref, st, {"Height"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"ApplyMode"})) wrote = write_number(ref, p, base, static_cast<double>(color.apply_mode)) || wrote;
        return wrote;
    }

    auto write_brush_settings(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, std::uintptr_t component, double radius) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        bool wrote = false;
        if (component)
        {
            const auto current = find_object_property(ref, component, "CurrentBrushSettings");
            const auto current_offset = current ? prop_offset(current) : -1;
            const auto dest_size = prop_element_size(prop);
            const auto source_size = current ? prop_element_size(current) : 0;
            const auto copy_size = source_size > 0 ? std::min(dest_size, source_size) : dest_size;
            if (current_offset >= 0 && copy_size > 0 && copy_size <= 1024 &&
                safe_copy(base, reinterpret_cast<void*>(component + current_offset), static_cast<std::size_t>(copy_size)))
            {
                wrote = true;
            }
        }
        const auto st = struct_type(ref, prop, {"Radius", "Hardness", "Opacity", "Spacing", "Falloff", "BlendMode", "Rotation"});
        if (!st)
        {
            return wrote;
        }
        if (const auto p = find_property_any(ref, st, {"Radius"})) wrote = write_number(ref, p, base, radius) || wrote;
        if (const auto p = find_property_any(ref, st, {"Hardness"})) wrote = write_number(ref, p, base, 1.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"Opacity"})) wrote = write_number(ref, p, base, 1.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"Spacing"})) wrote = write_number(ref, p, base, 0.25) || wrote;
        if (const auto p = find_property_any(ref, st, {"Falloff"})) wrote = write_number(ref, p, base, 2.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"BlendMode"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"Rotation"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        return wrote;
    }

    auto write_paint_stroke_data(Reflection& ref,
                                 std::uintptr_t prop,
                                 std::uint8_t* container,
                                 std::uintptr_t component,
                                 double u,
                                 double v,
                                 const Color& color,
                                 double radius) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        const auto st = struct_type(ref,
                                    prop,
                                    {"Uv",
                                     "UV",
                                     "PaintUv",
                                     "PaintUV",
                                     "BrushSettings",
                                     "ChannelData",
                                     "TargetChannel",
                                     "EffectiveBrushWorldRadius"});
        if (!st)
        {
            return false;
        }
        bool wrote = false;
        if (const auto p = find_property_any(ref, st, {"Uv", "UV", "PaintUv", "PaintUV"})) wrote = write_vector2(ref, p, base, u, v) || wrote;
        if (const auto p = find_property_any(ref, st, {"BrushSettings"})) wrote = write_brush_settings(ref, p, base, component, radius) || wrote;
        if (const auto p = find_property_any(ref, st, {"ChannelData"})) wrote = write_channel_data(ref, p, base, color, true) || wrote;
        if (const auto p = find_property_any(ref, st, {"TargetChannel", "Channel", "PaintChannel"})) wrote = write_number(ref, p, base, PaintChannelAlbedoMetallicRoughness) || wrote;
        if (const auto p = find_property_any(ref, st, {"EffectiveBrushWorldRadius"})) wrote = write_number(ref, p, base, radius) || wrote;
        if (const auto p = find_property_any(ref, st, {"EffectiveSubdivisionLevel"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"EffectiveSubdivisionPixelSize"})) wrote = write_number(ref, p, base, 1.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"EffectiveTemplateResolution"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"EffectiveMaxGeneratedBrushTriangles"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"EffectiveGutterExpandPixels"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, st, {"bHasWorldPosition"})) wrote = write_bool(p, base, false) || wrote;
        if (const auto p = find_property_any(ref, st, {"bHasLocalPosition"})) wrote = write_bool(p, base, false) || wrote;
        if (const auto p = find_property_any(ref, st, {"bHasSkeletalTriangleAnchor"})) wrote = write_bool(p, base, false) || wrote;
        return wrote;
    }

    auto process_event(std::uintptr_t object, std::uintptr_t function, std::uint8_t* params, std::string& failure) -> bool
    {
        auto target = g_original_process_event.load();
        if (!target)
        {
            const auto module = main_module_range();
            const auto sdk_target = module.base ? module.base + meccha_sdk::Offsets::ProcessEvent : 0;
            if (sdk_target && address_in_main_module(sdk_target))
            {
                target = sdk_target;
            }
        }
        if (!target)
        {
            const auto vtable = safe_read<std::uintptr_t>(object);
            if (!vtable)
            {
                failure = "vtable_unavailable";
                return false;
            }
            target = safe_read<std::uintptr_t>(vtable + static_cast<std::uintptr_t>(ProcessEventVtableIndex) * sizeof(std::uintptr_t));
            if (!target)
            {
                failure = "process_event_unavailable";
                return false;
            }
        }
        __try
        {
            reinterpret_cast<ProcessEventFn>(target)(reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            failure = "process_event_exception";
            return false;
        }
    }

    auto read_return_bool(Reflection& ref, std::uintptr_t function, std::uint8_t* params) -> bool
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            if (name == "ReturnValue")
            {
                const auto offset = prop_offset(prop);
                return offset < 0 ? true : (*(params + offset) != 0);
            }
        }
        return true;
    }

    auto read_return_object(Reflection& ref, std::uintptr_t function, std::uint8_t* params) -> std::uintptr_t
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            if (name == "ReturnValue")
            {
                const auto offset = prop_offset(prop);
                return offset < 0 ? 0 : safe_read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(params + offset));
            }
        }
        return 0;
    }

    auto call_no_params_return_object(Reflection& ref, std::uintptr_t object, const char* function_name) -> std::uintptr_t
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return 0;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 1024)
        {
            return 0;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(1, params_size)), 0);
        std::string failure{};
        if (!process_event(object, function, params.data(), failure))
        {
            return 0;
        }
        return read_return_object(ref, function, params.data());
    }

    auto call_no_params_return_bool(Reflection& ref, std::uintptr_t object, const char* function_name) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 1024)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(1, params_size)), 0);
        std::string failure{};
        if (!process_event(object, function, params.data(), failure))
        {
            return false;
        }
        return read_return_bool(ref, function, params.data());
    }

    auto read_object_property_by_names(Reflection& ref, std::uintptr_t object, std::initializer_list<const char*> names) -> std::uintptr_t
    {
        if (!live_uobject(object))
        {
            return 0;
        }
        for (const auto* name : names)
        {
            if (!name)
            {
                continue;
            }
            const auto prop = find_object_property(ref, object, name);
            const auto offset = prop ? prop_offset(prop) : -1;
            if (offset < 0)
            {
                continue;
            }
            const auto value = safe_read<std::uintptr_t>(object + offset);
            if (live_uobject(value))
            {
                return value;
            }
        }
        return 0;
    }

    auto call_uv_paint_function(Reflection& ref,
                                std::uintptr_t component,
                                const char* function_name,
                                double u,
                                double v,
                                const Color& color,
                                double radius,
                                bool allow_brush_settings,
                                std::string& failure) -> bool
    {
        const auto function = ref.find_function(component, function_name);
        if (!function)
        {
            failure = std::string(function_name) + "_unavailable";
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 8192)
        {
            failure = "paint_params_size_invalid";
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        bool wrote_uv = false;
        bool wrote_channel_data = false;
        bool wrote_channel = false;
        bool brush_required = false;
        bool wrote_brush = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto raw_name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            const auto name = lower_copy(raw_name);
            if (name == "returnvalue")
            {
                continue;
            }
            if (name == "uv" || contains_text(name, "paintuv"))
            {
                wrote_uv = write_vector2(ref, prop, params.data(), u, v) || wrote_uv;
                continue;
            }
            if (contains_text(name, "channeldata"))
            {
                wrote_channel_data = write_channel_data(ref, prop, params.data(), color, true) || wrote_channel_data;
                continue;
            }
            if (contains_text(name, "brushsettings"))
            {
                brush_required = true;
                if (allow_brush_settings)
                {
                    wrote_brush = write_brush_settings(ref, prop, params.data(), component, radius) || wrote_brush;
                }
                continue;
            }
            if (name == "channel" || name == "targetchannel" || contains_text(name, "paintchannel"))
            {
                wrote_channel = write_number(ref, prop, params.data(), PaintChannelAlbedoMetallicRoughness) || wrote_channel;
                continue;
            }
        }
        if (!wrote_uv || !wrote_channel_data || !wrote_channel || (brush_required && !wrote_brush))
        {
            failure = "paint_param_write_failed uv=" + std::to_string(wrote_uv ? 1 : 0) + " channel_data=" +
                      std::to_string(wrote_channel_data ? 1 : 0) + " channel=" + std::to_string(wrote_channel ? 1 : 0) +
                      " brush=" + std::to_string((!brush_required || wrote_brush) ? 1 : 0);
            return false;
        }
        if (!process_event(component, function, params.data(), failure))
        {
            return false;
        }
        if (!read_return_bool(ref, function, params.data()))
        {
            failure = "paint_return_false";
            return false;
        }
        return true;
    }

    auto call_stroke_paint_function(Reflection& ref,
                                    std::uintptr_t component,
                                    const char* function_name,
                                    double u,
                                    double v,
                                    const Color& color,
                                    double radius,
                                    std::string& failure) -> bool
    {
        const auto function = ref.find_function(component, function_name);
        if (!function)
        {
            failure = std::string(function_name) + "_unavailable";
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 8192)
        {
            failure = std::string(function_name) + "_params_size_invalid";
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        bool wrote_stroke = false;
        bool wrote_uv = false;
        bool wrote_channel_data = false;
        bool wrote_channel = false;
        bool wrote_brush = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto raw_name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            const auto name = lower_copy(raw_name);
            if (name == "returnvalue")
            {
                continue;
            }
            wrote_stroke = write_paint_stroke_data(ref, prop, params.data(), component, u, v, color, radius) || wrote_stroke;
            if (name == "uv" || contains_text(name, "paintuv"))
            {
                wrote_uv = write_vector2(ref, prop, params.data(), u, v) || wrote_uv;
                continue;
            }
            if (contains_text(name, "channeldata"))
            {
                wrote_channel_data = write_channel_data(ref, prop, params.data(), color, true) || wrote_channel_data;
                continue;
            }
            if (contains_text(name, "brushsettings"))
            {
                wrote_brush = write_brush_settings(ref, prop, params.data(), component, radius) || wrote_brush;
                continue;
            }
            if (name == "channel" || name == "targetchannel" || contains_text(name, "paintchannel"))
            {
                wrote_channel = write_number(ref, prop, params.data(), PaintChannelAlbedoMetallicRoughness) || wrote_channel;
                continue;
            }
        }
        if (!wrote_stroke && (!wrote_uv || !wrote_channel_data))
        {
            failure = std::string(function_name) + "_param_write_failed stroke=" + std::to_string(wrote_stroke ? 1 : 0) +
                      " uv=" + std::to_string(wrote_uv ? 1 : 0) + " channel_data=" + std::to_string(wrote_channel_data ? 1 : 0) +
                      " channel=" + std::to_string(wrote_channel ? 1 : 0) + " brush=" + std::to_string(wrote_brush ? 1 : 0);
            return false;
        }
        if (!process_event(component, function, params.data(), failure))
        {
            failure = std::string(function_name) + "_" + failure;
            return false;
        }
        if (!read_return_bool(ref, function, params.data()))
        {
            failure = std::string(function_name) + "_return_false";
            return false;
        }
        return true;
    }

    auto call_replicated_paint_at_uv(Reflection& ref,
                                     std::uintptr_t component,
                                     double u,
                                     double v,
                                     const Color& color,
                                     double radius,
                                     std::string& failure,
                                     PaintCallStats& stats) -> bool
    {
        bool called = false;
        std::string first_failure{};
        const char* server_rpcs[]{"ServerSendPaint", "ServerPaint", "SendPaintToServer", "RequestPaintOnServer"};
        for (const auto* rpc : server_rpcs)
        {
            std::string rpc_failure{};
            if (call_stroke_paint_function(ref, component, rpc, u, v, color, radius, rpc_failure))
            {
                called = true;
                ++stats.server_success;
                break;
            }
            ++stats.server_failure;
            if (first_failure.empty())
            {
                first_failure = rpc_failure;
            }
        }

        bool local_called = false;
        const char* local_stroke_rpcs[]{"PaintStrokeUV"};
        for (const auto* rpc : local_stroke_rpcs)
        {
            std::string rpc_failure{};
            if (call_stroke_paint_function(ref, component, rpc, u, v, color, radius, rpc_failure))
            {
                local_called = true;
                ++stats.local_success;
                break;
            }
            ++stats.local_failure;
            if (first_failure.empty())
            {
                first_failure = rpc_failure;
            }
        }

        const char* local_uv_rpcs[]{"PaintAtUVWithBrush", "PaintAtUV"};
        for (const auto* rpc : local_uv_rpcs)
        {
            std::string rpc_failure{};
            if (call_uv_paint_function(ref, component, rpc, u, v, color, radius, true, rpc_failure))
            {
                local_called = true;
                ++stats.local_success;
                break;
            }
            ++stats.local_failure;
            if (first_failure.empty())
            {
                first_failure = rpc_failure;
            }
        }

        if (called || local_called)
        {
            return true;
        }
        failure = !first_failure.empty() ? first_failure : "replicated_paint_rpc_unavailable";
        if (stats.first_failure.empty())
        {
            stats.first_failure = failure;
        }
        return false;
    }

    auto parse_average_color(const std::string& request) -> Color
    {
        Color out{};
        out.r = 0.42;
        out.g = 0.42;
        out.b = 0.36;
        out.roughness = 0.65;
        out.metallic = 0.0;
        out.apply_mode = 1;

        double sr = 0.0, sg = 0.0, sb = 0.0;
        int count = 0;
        std::size_t pos = request.find("\"front_samples\"");
        if (pos == std::string::npos)
        {
            pos = 0;
        }
        while (count < 256)
        {
            const auto rp = request.find("\"r\":", pos);
            if (rp == std::string::npos)
            {
                break;
            }
            const auto gp = request.find("\"g\":", rp);
            const auto bp = request.find("\"b\":", gp == std::string::npos ? rp : gp);
            if (gp == std::string::npos || bp == std::string::npos)
            {
                break;
            }
            char* end = nullptr;
            const double r = std::strtod(request.c_str() + rp + 4, &end);
            const double g = std::strtod(request.c_str() + gp + 4, &end);
            const double b = std::strtod(request.c_str() + bp + 4, &end);
            if (std::isfinite(r) && std::isfinite(g) && std::isfinite(b))
            {
                sr += clamp01(r);
                sg += clamp01(g);
                sb += clamp01(b);
                ++count;
            }
            pos = bp + 4;
        }
        if (count > 0)
        {
            out.r = sr / count;
            out.g = sg / count;
            out.b = sb / count;
        }
        return out;
    }

    struct ComponentSelection
    {
        std::uintptr_t component{0};
        std::uintptr_t pawn{0};
        std::uintptr_t target{0};
        std::uintptr_t target_mesh{0};
        std::uintptr_t owner{0};
        std::string source{};
        std::string target_source{};
        std::string mesh_source{};
    };

    auto hex_address(std::uintptr_t value) -> std::string
    {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
        return buffer;
    }

    auto find_component(Reflection& ref, std::string& failure) -> ComponentSelection
    {
        ComponentSelection selected{};
        const auto root_component_offset = ref.resolve_property_offset("Actor", "RootComponent");
        const auto attach_children_offset = ref.resolve_property_offset("SceneComponent", "AttachChildren");
        const auto owned_components_offset = ref.resolve_property_offset("Actor", "OwnedComponents");
        int owner_offset = ref.resolve_property_offset("ActorComponent", "OwnerPrivate");
        if (owner_offset < 0)
        {
            owner_offset = ref.resolve_property_offset("ActorComponent", "Owner");
        }

        const auto engine = ref.find_first_instance("GameEngine");
        const auto viewport_offset = ref.resolve_property_offset("Engine", "GameViewport");
        const auto world_offset = ref.resolve_property_offset("GameViewportClient", "World");
        const auto game_instance_offset = ref.resolve_property_offset("World", "OwningGameInstance");
        const auto local_players_offset = ref.resolve_property_offset("GameInstance", "LocalPlayers");
        int controller_offset = ref.resolve_property_offset("Player", "PlayerController");
        if (controller_offset < 0)
        {
            controller_offset = ref.resolve_property_offset("LocalPlayer", "PlayerController");
        }
        int pawn_offset = ref.resolve_property_offset("PlayerController", "AcknowledgedPawn");
        if (pawn_offset < 0)
        {
            pawn_offset = ref.resolve_property_offset("Controller", "Pawn");
        }
        const auto viewport = viewport_offset >= 0 ? safe_read<std::uintptr_t>(engine + viewport_offset) : 0;
        const auto world = world_offset >= 0 ? safe_read<std::uintptr_t>(viewport + world_offset) : 0;
        const auto game_instance = game_instance_offset >= 0 ? safe_read<std::uintptr_t>(world + game_instance_offset) : 0;
        const auto local_players_data = local_players_offset >= 0 ? safe_read<std::uintptr_t>(game_instance + local_players_offset) : 0;
        const auto local_players_count = local_players_offset >= 0 ? safe_read<int>(game_instance + local_players_offset + 8) : 0;
        const auto local_player = local_players_data && local_players_count > 0 ? safe_read<std::uintptr_t>(local_players_data) : 0;
        auto controller = controller_offset >= 0 ? safe_read<std::uintptr_t>(local_player + controller_offset) : 0;
        auto pawn = pawn_offset >= 0 ? safe_read<std::uintptr_t>(controller + pawn_offset) : 0;
        auto read_controller_pawn = [&](std::uintptr_t candidate_controller) -> std::uintptr_t {
            if (!live_uobject(candidate_controller))
            {
                return 0;
            }
            if (pawn_offset >= 0)
            {
                if (const auto candidate_pawn = safe_read<std::uintptr_t>(candidate_controller + pawn_offset); live_uobject(candidate_pawn))
                {
                    return candidate_pawn;
                }
            }
            const auto candidate_pawn = call_no_params_return_object(ref, candidate_controller, "GetPawn");
            return live_uobject(candidate_pawn) ? candidate_pawn : 0;
        };
        if (!live_uobject(pawn))
        {
            pawn = read_controller_pawn(controller);
        }
        if (!live_uobject(pawn))
        {
            ref.for_each_object([&](std::uintptr_t obj) {
                if (!live_uobject(obj))
                {
                    return false;
                }
                const auto cls = lower_copy(ref.class_name(obj));
                if (!contains_text(cls, "playercontroller"))
                {
                    return false;
                }
                if (const auto candidate_pawn = read_controller_pawn(obj))
                {
                    controller = obj;
                    pawn = candidate_pawn;
                    return true;
                }
                return false;
            });
        }
        selected.pawn = pawn;
        const auto controller_view_target = live_uobject(controller) ? call_no_params_return_object(ref, controller, "GetViewTarget") : 0;
        const auto camera = live_uobject(controller) ? call_no_params_return_object(ref, controller, "GetPlayerCameraManager") : 0;
        const auto camera_view_target = live_uobject(camera) ? call_no_params_return_object(ref, camera, "GetViewTarget") : 0;
        std::vector<std::pair<std::uintptr_t, const char*>> targets{};
        auto add_target = [&](std::uintptr_t object, const char* source) {
            if (!live_uobject(object))
            {
                return;
            }
            for (const auto& existing : targets)
            {
                if (existing.first == object)
                {
                    return;
                }
            }
            targets.push_back({object, source});
        };
        add_target(controller_view_target, "controller_view_target");
        add_target(camera_view_target, "camera_view_target");
        add_target(pawn, "controller_pawn");

        auto read_owner = [&](std::uintptr_t obj) -> std::uintptr_t {
            if (owner_offset >= 0)
            {
                if (const auto owner = safe_read<std::uintptr_t>(obj + owner_offset))
                {
                    return owner;
                }
            }
            return call_no_params_return_object(ref, obj, "GetOwner");
        };

        auto live_object = [&](std::uintptr_t obj) -> bool {
            return live_uobject(obj);
        };

        auto owner_matches_target = [&](std::uintptr_t owner) -> bool {
            for (const auto& target : targets)
            {
                if (owner && owner == target.first)
                {
                    return true;
                }
            }
            return false;
        };

        auto target_source_for_owner = [&](std::uintptr_t owner) -> const char* {
            for (const auto& target : targets)
            {
                if (owner && owner == target.first)
                {
                    return target.second;
                }
            }
            return "";
        };

        auto outer_matches_target = [&](std::uintptr_t object, std::uintptr_t& matched_target, const char*& matched_source) -> bool {
            for (int depth = 0; live_uobject(object) && depth < 8; ++depth)
            {
                const auto outer = safe_read<std::uintptr_t>(object + OffOuter);
                if (!live_uobject(outer))
                {
                    return false;
                }
                if (owner_matches_target(outer))
                {
                    matched_target = outer;
                    matched_source = target_source_for_owner(outer);
                    return true;
                }
                object = outer;
            }
            return false;
        };

        std::vector<std::pair<std::uintptr_t, const char*>> target_meshes{};
        auto add_target_mesh = [&](std::uintptr_t mesh, const char* source) {
            if (!live_uobject(mesh))
            {
                return;
            }
            const auto cls = lower_copy(ref.class_name(mesh));
            if (!contains_text(cls, "mesh"))
            {
                return;
            }
            for (const auto& existing : target_meshes)
            {
                if (existing.first == mesh)
                {
                    return;
                }
            }
            target_meshes.push_back({mesh, source});
        };

        auto collect_meshes_from_actor = [&](std::uintptr_t actor, const char* source) {
            add_target_mesh(read_object_property_by_names(ref,
                                                          actor,
                                                          {"Mesh", "MeshComponent", "SkeletalMeshComponent", "TargetMeshComponent", "TargetMesh"}),
                            source);
            if (root_component_offset >= 0 && attach_children_offset >= 0)
            {
                const auto root = safe_read<std::uintptr_t>(actor + root_component_offset);
                const auto data = safe_read<std::uintptr_t>(root + attach_children_offset);
                const auto count = safe_read<int>(root + attach_children_offset + 8);
                if (data && count > 0 && count <= 512)
                {
                    for (int i = 0; i < count; ++i)
                    {
                        add_target_mesh(safe_read<std::uintptr_t>(data + static_cast<std::uintptr_t>(i) * 8), source);
                    }
                }
            }
            if (owned_components_offset >= 0)
            {
                const auto data = safe_read<std::uintptr_t>(actor + owned_components_offset);
                const auto count = safe_read<int>(actor + owned_components_offset + 8);
                if (data && count > 0 && count <= 512)
                {
                    for (int i = 0; i < count; ++i)
                    {
                        add_target_mesh(safe_read<std::uintptr_t>(data + static_cast<std::uintptr_t>(i) * 8), source);
                    }
                }
            }
        };

        for (const auto& target : targets)
        {
            collect_meshes_from_actor(target.first, target.second);
        }

        ref.for_each_object([&](std::uintptr_t obj) {
            if (!live_uobject(obj))
            {
                return false;
            }
            const auto cls = lower_copy(ref.class_name(obj));
            if (!contains_text(cls, "meshcomponent"))
            {
                return false;
            }
            const auto owner = read_owner(obj);
            if (owner_matches_target(owner))
            {
                add_target_mesh(obj, target_source_for_owner(owner));
            }
            return false;
        });

        auto mesh_match_source = [&](std::uintptr_t mesh) -> const char* {
            for (const auto& target_mesh : target_meshes)
            {
                if (mesh && mesh == target_mesh.first)
                {
                    return target_mesh.second;
                }
            }
            return "";
        };

        auto property_reference_matches = [&](std::uintptr_t object,
                                              std::uintptr_t& matched_target,
                                              const char*& matched_target_source,
                                              std::uintptr_t& matched_mesh,
                                              const char*& matched_mesh_source) -> bool {
            auto cls = ref.class_ptr(object);
            for (int depth = 0; cls && depth < 32; ++depth)
            {
                for (auto prop = safe_read<std::uintptr_t>(cls + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
                {
                    const auto offset = prop_offset(prop);
                    if (offset < 0 || offset > 0x10000)
                    {
                        continue;
                    }
                    const auto value = safe_read<std::uintptr_t>(object + offset);
                    if (!live_uobject(value))
                    {
                        continue;
                    }
                    if (owner_matches_target(value))
                    {
                        matched_target = value;
                        matched_target_source = target_source_for_owner(value);
                        return true;
                    }
                    const auto* source = mesh_match_source(value);
                    if (source && source[0] != '\0')
                    {
                        matched_mesh = value;
                        matched_mesh_source = source;
                        return true;
                    }
                }
                cls = safe_read<std::uintptr_t>(cls + OffSuperStruct);
            }
            return false;
        };

        auto component_matches_target_mesh = [&](std::uintptr_t obj, std::uintptr_t& matched_mesh, const char*& matched_source) -> bool {
            const auto mesh = read_object_property_by_names(ref,
                                                            obj,
                                                            {"TargetMeshComponent",
                                                             "TargetMesh",
                                                             "MeshComponent",
                                                             "SkeletalMeshComponent",
                                                             "Mesh",
                                                             "OwnerMesh"});
            const auto* source = mesh_match_source(mesh);
            if (mesh && source && source[0] != '\0')
            {
                matched_mesh = mesh;
                matched_source = source;
                return true;
            }
            return false;
        };

        int candidate_count = 0;
        int owner_match_count = 0;
        int outer_match_count = 0;
        int ref_match_count = 0;
        int mesh_match_count = 0;
        int any_owner_candidate_count = 0;
        std::string first_export_probe_failure{};
        std::uintptr_t first_export_probe_component = 0;
        std::string first_export_probe_class{};

        auto check_component = [&](std::uintptr_t obj, const char* source, bool require_owner) -> bool {
            if (!live_object(obj))
            {
                return false;
            }
            const auto cls = lower_copy(ref.class_name(obj));
            if ((contains_text(cls, "runtimepaint") || contains_text(cls, "paint")) && ref.find_function(obj, "PaintAtUVWithBrush"))
            {
                ++candidate_count;
                const auto owner = read_owner(obj);
                const bool owner_match = owner_matches_target(owner);
                std::uintptr_t matched_outer_target = 0;
                const char* matched_outer_source = "";
                const bool outer_match = outer_matches_target(obj, matched_outer_target, matched_outer_source);
                std::uintptr_t matched_ref_target = 0;
                const char* matched_ref_target_source = "";
                std::uintptr_t matched_mesh = 0;
                const char* matched_mesh_source = "";
                bool mesh_match = component_matches_target_mesh(obj, matched_mesh, matched_mesh_source);
                const bool ref_match = property_reference_matches(obj, matched_ref_target, matched_ref_target_source, matched_mesh, matched_mesh_source);
                mesh_match = mesh_match || (matched_mesh != 0);
                if (owner_match)
                {
                    ++owner_match_count;
                }
                if (outer_match)
                {
                    ++outer_match_count;
                }
                if (ref_match)
                {
                    ++ref_match_count;
                }
                if (mesh_match)
                {
                    ++mesh_match_count;
                }
                if (require_owner && !owner_match && !outer_match && !ref_match && !mesh_match)
                {
                    return false;
                }
                if (require_owner)
                {
                    const auto export_probe = export_channel_bytes(ref, obj, 0);
                    if (!export_probe.ok)
                    {
                        if (first_export_probe_failure.empty())
                        {
                            first_export_probe_failure = export_probe.failure;
                            first_export_probe_component = obj;
                            first_export_probe_class = ref.class_name(obj);
                        }
                        return false;
                    }
                }
                selected.component = obj;
                selected.owner = owner;
                selected.target = owner_match ? owner : (outer_match ? matched_outer_target : matched_ref_target);
                selected.target_source = owner_match ? target_source_for_owner(owner) : (outer_match ? matched_outer_source : matched_ref_target_source);
                selected.target_mesh = matched_mesh;
                selected.mesh_source = matched_mesh ? matched_mesh_source : "";
                selected.source = source ? source : "unknown";
                return true;
            }
            return false;
        };

        for (const auto& target : targets)
        {
            if (root_component_offset >= 0 && attach_children_offset >= 0)
            {
                const auto root = safe_read<std::uintptr_t>(target.first + root_component_offset);
                const auto data = safe_read<std::uintptr_t>(root + attach_children_offset);
                const auto count = safe_read<int>(root + attach_children_offset + 8);
                if (data && count > 0 && count <= 512)
                {
                    for (int i = 0; i < count; ++i)
                    {
                        if (check_component(safe_read<std::uintptr_t>(data + static_cast<std::uintptr_t>(i) * 8), "root_attach_children", false))
                        {
                            selected.target = target.first;
                            selected.target_source = target.second;
                            return selected;
                        }
                    }
                }
            }
            if (owned_components_offset >= 0)
            {
                const auto data = safe_read<std::uintptr_t>(target.first + owned_components_offset);
                const auto count = safe_read<int>(target.first + owned_components_offset + 8);
                if (data && count > 0 && count <= 512)
                {
                    for (int i = 0; i < count; ++i)
                    {
                        if (check_component(safe_read<std::uintptr_t>(data + static_cast<std::uintptr_t>(i) * 8), "owned_components", false))
                        {
                            selected.target = target.first;
                            selected.target_source = target.second;
                            return selected;
                        }
                    }
                }
            }
        }

        struct OwnedComponentCandidate
        {
            std::uintptr_t component{0};
            std::uintptr_t owner{0};
            std::uintptr_t mesh{0};
            std::string mesh_source{};
            int score{-1000000};
        };
        OwnedComponentCandidate best_owned{};
        ref.for_each_object([&](std::uintptr_t obj) {
            if (!live_object(obj))
            {
                return false;
            }
            const auto cls = lower_copy(ref.class_name(obj));
            if (!(contains_text(cls, "runtimepaint") || contains_text(cls, "paint")) ||
                !ref.find_function(obj, "PaintAtUVWithBrush") ||
                !ref.find_function(obj, "ExportChannelToBytes") ||
                !ref.find_function(obj, "ImportChannelFromBytes"))
            {
                return false;
            }
            const auto owner = read_owner(obj);
            if (!live_uobject(owner))
            {
                return false;
            }
            ++any_owner_candidate_count;
            const auto owner_cls = lower_copy(ref.class_name(owner));
            int score = 10;
            if (owner_matches_target(owner))
            {
                score += 1000;
            }
            if (call_no_params_return_bool(ref, owner, "IsPlayerControlled"))
            {
                score += 250;
            }
            if (contains_text(owner_cls, "character"))
            {
                score += 80;
            }
            if (contains_text(owner_cls, "pawn"))
            {
                score += 60;
            }
            if (call_no_params_return_object(ref, owner, "GetController"))
            {
                score += 25;
            }
            std::uintptr_t matched_mesh = 0;
            const char* matched_mesh_source = "";
            if (component_matches_target_mesh(obj, matched_mesh, matched_mesh_source))
            {
                score += 40;
            }
            if (score > best_owned.score)
            {
                best_owned.component = obj;
                best_owned.owner = owner;
                best_owned.mesh = matched_mesh;
                best_owned.mesh_source = matched_mesh_source ? matched_mesh_source : "";
                best_owned.score = score;
            }
            return false;
        });
        if (best_owned.component && best_owned.score >= 10)
        {
            selected.component = best_owned.component;
            selected.owner = best_owned.owner;
            selected.target = best_owned.owner;
            selected.target_source = owner_matches_target(best_owned.owner) ? target_source_for_owner(best_owned.owner) : "owned_runtimepaint_owner_scan";
            selected.target_mesh = best_owned.mesh;
            selected.mesh_source = best_owned.mesh_source;
            selected.source = "owned_runtimepaint_owner_scan";
            selected.pawn = best_owned.owner;
            return selected;
        }

        ref.for_each_object([&](std::uintptr_t obj) {
            return check_component(obj, "owned_runtimepaint_scan", true);
        });
        if (selected.component)
        {
            return selected;
        }
        if (!selected.component)
        {
            failure = "runtime_paint_component_unavailable pawn=" + hex_address(pawn) +
                      " view_target=" + hex_address(controller_view_target) +
                      " camera_view_target=" + hex_address(camera_view_target) +
                      " meshes=" + std::to_string(target_meshes.size()) +
                      " candidates=" + std::to_string(candidate_count) +
                      " any_owner_candidates=" + std::to_string(any_owner_candidate_count) +
                      " owner_matches=" + std::to_string(owner_match_count) +
                      " outer_matches=" + std::to_string(outer_match_count) +
                      " ref_matches=" + std::to_string(ref_match_count) +
                      " mesh_matches=" + std::to_string(mesh_match_count) +
                      " export_probe_component=" + hex_address(first_export_probe_component) +
                      " export_probe_class=" + first_export_probe_class +
                      " export_probe_failure=" + first_export_probe_failure;
        }
        return selected;
    }

    auto install_process_event_hook(std::string& failure) -> bool
    {
        if (g_process_event_hook_installed.load())
        {
            return true;
        }
        DWORD thread_id = 0;
        const DWORD process_id = GetCurrentProcessId();
        EnumWindows(
            [](HWND hwnd, LPARAM lparam) -> BOOL {
                DWORD owner_pid = 0;
                const DWORD tid = GetWindowThreadProcessId(hwnd, &owner_pid);
                if (owner_pid == GetCurrentProcessId() && tid != 0 && IsWindowVisible(hwnd))
                {
                    *reinterpret_cast<DWORD*>(lparam) = tid;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&thread_id));
        if (thread_id == 0)
        {
            failure = "game_window_thread_unavailable pid=" + std::to_string(process_id);
            return false;
        }
        const auto hook = SetWindowsHookExW(WH_GETMESSAGE, message_hook_proc, g_module, thread_id);
        if (!hook)
        {
            failure = "message_hook_install_failed win32=" + std::to_string(GetLastError()) + " thread=" + std::to_string(thread_id);
            return false;
        }
        g_message_hook.store(hook);
        g_game_thread_id.store(thread_id);
        g_process_event_hook_installed.store(true);
        PostThreadMessageW(thread_id, PaintDispatchMessage, 0, 0);
        return true;
    }

    auto uninstall_process_event_hook() -> void
    {
        const auto message_hook = g_message_hook.exchange(nullptr);
        if (message_hook)
        {
            UnhookWindowsHookEx(message_hook);
        }
        g_game_thread_id.store(0);
        const auto hook = reinterpret_cast<std::uintptr_t>(&hooked_process_event);
        std::lock_guard<std::mutex> hook_lock(g_hook_mutex);
        for (const auto& entry : g_process_event_hook_slots)
        {
            const auto slot_address = entry.first;
            const auto original = entry.second;
            auto* slot = reinterpret_cast<std::uintptr_t*>(slot_address);
            DWORD old_protect = 0;
            if (VirtualProtect(slot, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &old_protect))
            {
                if (safe_read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(slot)) == hook)
                {
                    *slot = original;
                    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(std::uintptr_t));
                }
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(std::uintptr_t), old_protect, &ignored);
            }
        }
        g_process_event_hook_slots.clear();
        g_original_process_event.store(0);
        g_process_event_hook_installed.store(false);
    }

    auto json_bool(bool value) -> const char*
    {
        return value ? "true" : "false";
    }

    auto first_bytes_hex(const std::vector<std::uint8_t>& bytes, std::size_t limit = 16) -> std::string
    {
        static const char* hex = "0123456789abcdef";
        std::string out{};
        const auto count = std::min(limit, bytes.size());
        out.reserve(count * 2);
        for (std::size_t i = 0; i < count; ++i)
        {
            out.push_back(hex[(bytes[i] >> 4) & 0xF]);
            out.push_back(hex[bytes[i] & 0xF]);
        }
        return out;
    }

    struct SdkContext
    {
        bool ok{false};
        std::string stage{"sdk_unavailable"};
        std::string message{"sdk unavailable"};
        std::uintptr_t module_base{0};
        std::uintptr_t expected_guobject_array{0};
        std::uintptr_t actual_guobject_array{0};
        std::uintptr_t expected_gworld{0};
        std::uintptr_t process_event{0};
        std::uintptr_t world{0};
        std::uintptr_t game_instance{0};
        int local_players_count{0};
        std::uintptr_t local_player{0};
        std::uintptr_t controller{0};
        std::uintptr_t k2_get_pawn_function{0};
        std::uintptr_t pawn{0};
        std::uintptr_t k2_get_actor_location_function{0};
        meccha_sdk::FVector body_world_position{};
        std::uintptr_t component{0};
        std::uintptr_t relay_component{0};
        std::uintptr_t relay_stroke_batch_to_server_function{0};
        std::uintptr_t relay_paint_to_server_function{0};
        std::uintptr_t export_function{0};
        std::uintptr_t import_function{0};
        std::uintptr_t get_render_target_function{0};
        std::uintptr_t server_paint_batch_function{0};
        std::uintptr_t server_send_stroke_batch_function{0};
        std::uintptr_t server_send_paint_function{0};
        std::uintptr_t server_paint_function{0};
        std::uintptr_t paint_at_uv_with_brush_function{0};
    };

    struct SdkViewportInfo
    {
        int width{0};
        int height{0};
    };

    struct SdkDeprojectRay
    {
        bool ok{false};
        std::string failure{};
        meccha_sdk::FVector location{};
        meccha_sdk::FVector direction{};
    };

    struct SdkBrushQueryHit
    {
        bool params_ok{false};
        bool success{false};
        bool has_uv{false};
        double u{0.0};
        double v{0.0};
        meccha_sdk::FVector world_position{};
        meccha_sdk::FVector normal{};
        std::uintptr_t actor{0};
        std::uintptr_t component{0};
        std::string failure{};
    };

    auto sdk_vec_add(const meccha_sdk::FVector& a, const meccha_sdk::FVector& b) -> meccha_sdk::FVector
    {
        return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
    }

    auto sdk_vec_sub(const meccha_sdk::FVector& a, const meccha_sdk::FVector& b) -> meccha_sdk::FVector
    {
        return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
    }

    auto sdk_vec_mul(const meccha_sdk::FVector& a, double scale) -> meccha_sdk::FVector
    {
        return {a.X * scale, a.Y * scale, a.Z * scale};
    }

    auto sdk_vec_dot(const meccha_sdk::FVector& a, const meccha_sdk::FVector& b) -> double
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    auto sdk_vec_cross(const meccha_sdk::FVector& a, const meccha_sdk::FVector& b) -> meccha_sdk::FVector
    {
        return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
    }

    auto sdk_vec_len(const meccha_sdk::FVector& a) -> double
    {
        return std::sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z);
    }

    auto sdk_vec_normalize(const meccha_sdk::FVector& a) -> meccha_sdk::FVector
    {
        const auto len = sdk_vec_len(a);
        if (len <= 0.000001)
        {
            return {};
        }
        return {a.X / len, a.Y / len, a.Z / len};
    }

    auto sdk_read_number(Reflection& ref, std::uintptr_t prop, std::uint8_t* container) -> double
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return 0.0;
        }
        auto* src = container + offset;
        const auto size = prop_element_size(prop);
        const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
        if (size == 8 && !contains_text(name, "int") && !contains_text(name, "channel") && !contains_text(name, "mode"))
        {
            return *reinterpret_cast<double*>(src);
        }
        if (size >= 4)
        {
            if (contains_text(name, "size") || contains_text(name, "width") || contains_text(name, "height") ||
                contains_text(name, "count") || contains_text(name, "index") || contains_text(name, "channel") ||
                contains_text(name, "mode"))
            {
                return static_cast<double>(*reinterpret_cast<std::int32_t*>(src));
            }
            return static_cast<double>(*reinterpret_cast<float*>(src));
        }
        if (size == 2)
        {
            return static_cast<double>(*reinterpret_cast<std::int16_t*>(src));
        }
        if (size == 1)
        {
            return static_cast<double>(*src);
        }
        return 0.0;
    }

    auto sdk_read_bool(std::uintptr_t prop, std::uint8_t* container) -> bool
    {
        const auto offset = prop_offset(prop);
        return offset >= 0 && *(container + offset) != 0;
    }

    auto sdk_write_object(std::uintptr_t prop, std::uint8_t* container, std::uintptr_t value) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        *reinterpret_cast<std::uintptr_t*>(container + offset) = value;
        return true;
    }

    auto sdk_read_object(std::uintptr_t prop, std::uint8_t* container) -> std::uintptr_t
    {
        const auto offset = prop_offset(prop);
        return offset < 0 ? 0 : safe_read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(container + offset));
    }

    auto sdk_write_vector3(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, const meccha_sdk::FVector& value) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        const auto st = struct_type(ref, prop, {"X", "Y", "Z"});
        bool wrote = false;
        if (st)
        {
            const auto x = find_property_any(ref, st, {"X"});
            const auto y = find_property_any(ref, st, {"Y"});
            const auto z = find_property_any(ref, st, {"Z"});
            const auto xo = x ? prop_offset(x) : -1;
            const auto yo = y ? prop_offset(y) : -1;
            const auto zo = z ? prop_offset(z) : -1;
            if (xo >= 0 && yo > xo && zo > yo)
            {
                if (yo - xo >= 8 && zo - yo >= 8)
                {
                    *reinterpret_cast<double*>(base + xo) = value.X;
                    *reinterpret_cast<double*>(base + yo) = value.Y;
                    *reinterpret_cast<double*>(base + zo) = value.Z;
                }
                else
                {
                    *reinterpret_cast<float*>(base + xo) = static_cast<float>(value.X);
                    *reinterpret_cast<float*>(base + yo) = static_cast<float>(value.Y);
                    *reinterpret_cast<float*>(base + zo) = static_cast<float>(value.Z);
                }
                return true;
            }
        }
        const auto size = prop_element_size(prop);
        if (size >= 24)
        {
            const double values[3]{value.X, value.Y, value.Z};
            std::memcpy(base, values, sizeof(values));
            return true;
        }
        if (size >= 12)
        {
            const float values[3]{static_cast<float>(value.X), static_cast<float>(value.Y), static_cast<float>(value.Z)};
            std::memcpy(base, values, sizeof(values));
            return true;
        }
        return false;
    }

    auto sdk_read_vector3(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, meccha_sdk::FVector& out) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        const auto st = struct_type(ref, prop, {"X", "Y", "Z"});
        if (st)
        {
            const auto x = find_property_any(ref, st, {"X"});
            const auto y = find_property_any(ref, st, {"Y"});
            const auto z = find_property_any(ref, st, {"Z"});
            const auto xo = x ? prop_offset(x) : -1;
            const auto yo = y ? prop_offset(y) : -1;
            const auto zo = z ? prop_offset(z) : -1;
            if (xo >= 0 && yo > xo && zo > yo)
            {
                if (yo - xo >= 8 && zo - yo >= 8)
                {
                    out.X = *reinterpret_cast<double*>(base + xo);
                    out.Y = *reinterpret_cast<double*>(base + yo);
                    out.Z = *reinterpret_cast<double*>(base + zo);
                }
                else
                {
                    out.X = *reinterpret_cast<float*>(base + xo);
                    out.Y = *reinterpret_cast<float*>(base + yo);
                    out.Z = *reinterpret_cast<float*>(base + zo);
                }
                return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
            }
        }
        const auto size = prop_element_size(prop);
        if (size >= 24)
        {
            const auto* values = reinterpret_cast<double*>(base);
            out = {values[0], values[1], values[2]};
            return true;
        }
        if (size >= 12)
        {
            const auto* values = reinterpret_cast<float*>(base);
            out = {values[0], values[1], values[2]};
            return true;
        }
        return false;
    }

    auto sdk_read_vector2(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, double& x, double& y) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        const auto st = struct_type(ref, prop, {"X", "Y"});
        if (st)
        {
            const auto xp = find_property_any(ref, st, {"X"});
            const auto yp = find_property_any(ref, st, {"Y"});
            const auto xo = xp ? prop_offset(xp) : -1;
            const auto yo = yp ? prop_offset(yp) : -1;
            if (xo >= 0 && yo > xo)
            {
                if (yo - xo >= 8)
                {
                    x = *reinterpret_cast<double*>(base + xo);
                    y = *reinterpret_cast<double*>(base + yo);
                }
                else
                {
                    x = *reinterpret_cast<float*>(base + xo);
                    y = *reinterpret_cast<float*>(base + yo);
                }
                return std::isfinite(x) && std::isfinite(y);
            }
        }
        const auto size = prop_element_size(prop);
        if (size >= 16)
        {
            const auto* values = reinterpret_cast<double*>(base);
            x = values[0];
            y = values[1];
            return true;
        }
        if (size >= 8)
        {
            const auto* values = reinterpret_cast<float*>(base);
            x = values[0];
            y = values[1];
            return true;
        }
        return false;
    }

    auto sdk_call_no_params(Reflection& ref, std::uintptr_t object, const char* function_name) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 1024)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(1, params_size)), 0);
        std::string failure{};
        return process_event(object, function, params.data(), failure);
    }

    auto sdk_call_single_number(Reflection& ref, std::uintptr_t object, const char* function_name, double value) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 1024)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        bool wrote = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            if (name != "ReturnValue")
            {
                wrote = write_number(ref, prop, params.data(), value) || wrote;
            }
        }
        std::string failure{};
        return wrote && process_event(object, function, params.data(), failure);
    }

    auto sdk_call_single_bool(Reflection& ref, std::uintptr_t object, const char* function_name, bool value) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 1024)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        bool wrote = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            if (name != "ReturnValue")
            {
                wrote = write_bool(prop, params.data(), value) || wrote;
            }
        }
        std::string failure{};
        return wrote && process_event(object, function, params.data(), failure);
    }

    auto sdk_call_object_param(Reflection& ref, std::uintptr_t object, const char* function_name, std::uintptr_t value) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function || !value)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 1024)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        bool wrote = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName));
            if (name != "ReturnValue")
            {
                wrote = sdk_write_object(prop, params.data(), value) || wrote;
            }
        }
        std::string failure{};
        return wrote && process_event(object, function, params.data(), failure);
    }

    auto sdk_context_metadata(Reflection& ref, const SdkContext& ctx) -> std::string
    {
        const auto guobject_delta = ctx.expected_guobject_array >= ctx.actual_guobject_array
            ? static_cast<long long>(ctx.expected_guobject_array - ctx.actual_guobject_array)
            : -static_cast<long long>(ctx.actual_guobject_array - ctx.expected_guobject_array);
        const bool guobject_offsets_match = ctx.expected_guobject_array != 0 && ctx.expected_guobject_array == ctx.actual_guobject_array;
        const bool guobject_offsets_compatible = guobject_offsets_match || guobject_delta == 0x10 || guobject_delta == -0x10;
        return "\"sdk_version\":\"chameleonEsp_dumper7_1_7_0_min\"" +
               std::string(",\"sdk_route\":\"sdk_server_paint_batch_strokes\"") +
               ",\"module_base\":\"" + hex_address(ctx.module_base) + "\"" +
               ",\"expected_guobject_array\":\"" + hex_address(ctx.expected_guobject_array) + "\"" +
               ",\"actual_guobject_array\":\"" + hex_address(ctx.actual_guobject_array) + "\"" +
               ",\"guobject_offset_delta\":" + std::to_string(guobject_delta) +
               ",\"guobject_offsets_match\":" + std::string(json_bool(guobject_offsets_match)) +
               ",\"guobject_offsets_compatible\":" + std::string(json_bool(guobject_offsets_compatible)) +
               ",\"expected_gworld\":\"" + hex_address(ctx.expected_gworld) + "\"" +
               ",\"process_event\":\"" + hex_address(ctx.process_event) + "\"" +
               ",\"process_event_offset\":\"0x15d09f0\"" +
               ",\"process_event_vtable_index\":" + std::to_string(ProcessEventVtableIndex) +
               ",\"world\":\"" + hex_address(ctx.world) + "\"" +
               ",\"game_instance\":\"" + hex_address(ctx.game_instance) + "\"" +
               ",\"local_players_count\":" + std::to_string(ctx.local_players_count) +
               ",\"local_player\":\"" + hex_address(ctx.local_player) + "\"" +
               ",\"controller\":\"" + hex_address(ctx.controller) + "\"" +
               ",\"k2_get_pawn_function\":\"" + hex_address(ctx.k2_get_pawn_function) + "\"" +
               ",\"pawn\":\"" + hex_address(ctx.pawn) + "\"" +
               ",\"pawn_class\":\"" + json_escape(ref.class_name(ctx.pawn)) + "\"" +
               ",\"k2_get_actor_location_function\":\"" + hex_address(ctx.k2_get_actor_location_function) + "\"" +
               ",\"body_world_x\":" + std::to_string(ctx.body_world_position.X) +
               ",\"body_world_y\":" + std::to_string(ctx.body_world_position.Y) +
               ",\"body_world_z\":" + std::to_string(ctx.body_world_position.Z) +
               ",\"runtime_paintable_offset\":\"0xb68\"" +
               ",\"component\":\"" + hex_address(ctx.component) + "\"" +
               ",\"component_class\":\"" + json_escape(ref.class_name(ctx.component)) + "\"" +
               ",\"runtime_paint_relay_offset\":\"0x770\"" +
               ",\"relay_component\":\"" + hex_address(ctx.relay_component) + "\"" +
               ",\"relay_component_class\":\"" + json_escape(ref.class_name(ctx.relay_component)) + "\"" +
               ",\"function_relay_stroke_batch_to_server_available\":" + std::string(json_bool(ctx.relay_stroke_batch_to_server_function != 0)) +
               ",\"function_relay_paint_to_server_available\":" + std::string(json_bool(ctx.relay_paint_to_server_function != 0)) +
               ",\"function_export_available\":" + std::string(json_bool(ctx.export_function != 0)) +
               ",\"function_get_render_target_available\":" + std::string(json_bool(ctx.get_render_target_function != 0)) +
               ",\"function_server_paint_batch_available\":" + std::string(json_bool(ctx.server_paint_batch_function != 0)) +
               ",\"function_server_send_stroke_batch_available\":" + std::string(json_bool(ctx.server_send_stroke_batch_function != 0)) +
               ",\"function_server_send_paint_available\":" + std::string(json_bool(ctx.server_send_paint_function != 0)) +
               ",\"function_server_paint_available\":" + std::string(json_bool(ctx.server_paint_function != 0)) +
               ",\"function_paint_at_uv_with_brush_available\":" + std::string(json_bool(ctx.paint_at_uv_with_brush_function != 0)) +
               ",\"function_relay_stroke_batch_to_server\":\"" + hex_address(ctx.relay_stroke_batch_to_server_function) + "\"" +
               ",\"function_relay_paint_to_server\":\"" + hex_address(ctx.relay_paint_to_server_function) + "\"" +
               ",\"function_export\":\"" + hex_address(ctx.export_function) + "\"" +
               ",\"function_get_render_target\":\"" + hex_address(ctx.get_render_target_function) + "\"" +
               ",\"function_server_paint_batch\":\"" + hex_address(ctx.server_paint_batch_function) + "\"" +
               ",\"function_server_send_stroke_batch\":\"" + hex_address(ctx.server_send_stroke_batch_function) + "\"" +
               ",\"function_server_send_paint\":\"" + hex_address(ctx.server_send_paint_function) + "\"" +
               ",\"function_server_paint\":\"" + hex_address(ctx.server_paint_function) + "\"" +
               ",\"function_paint_at_uv_with_brush\":\"" + hex_address(ctx.paint_at_uv_with_brush_function) + "\"" +
               ",\"param_schema\":\"FPaintStroke{Uv@0,WorldPosition@16,bHasWorldPosition@40,BrushSettings@104,ChannelData@144,TargetChannel@176};ServerPaintBatch{Batch@0};PaintAtUVWithBrush{Uv@0,ChannelData@16,BrushSettings@48,Channel@88}\"" +
               std::string(",\"replication\":\"component_server_paint_batch\"") +
               ",\"multiplayer_replicated\":true";
    }

    auto sdk_resolve_context(Reflection& ref) -> SdkContext
    {
        SdkContext ctx{};
        const auto module = main_module_range();
        ctx.module_base = module.base;
        ctx.actual_guobject_array = ref.guobject_array;
        if (!module.base)
        {
            ctx.stage = "sdk_unavailable";
            ctx.message = "main module unavailable";
            return ctx;
        }
        ctx.expected_guobject_array = module.base + meccha_sdk::Offsets::GObjects;
        ctx.expected_gworld = module.base + meccha_sdk::Offsets::GWorld;
        ctx.process_event = module.base + meccha_sdk::Offsets::ProcessEvent;
        const auto guobject_delta = ctx.expected_guobject_array >= ctx.actual_guobject_array
            ? static_cast<long long>(ctx.expected_guobject_array - ctx.actual_guobject_array)
            : -static_cast<long long>(ctx.actual_guobject_array - ctx.expected_guobject_array);
        const bool guobject_offsets_compatible = ctx.expected_guobject_array == ctx.actual_guobject_array ||
            guobject_delta == 0x10 || guobject_delta == -0x10;
        if (!guobject_offsets_compatible)
        {
            ctx.stage = "sdk_mismatch";
            ctx.message = "Dumper7 SDK offsets do not match current game build";
            return ctx;
        }
        if (!address_in_main_module(ctx.process_event))
        {
            ctx.stage = "sdk_mismatch";
            ctx.message = "Dumper7 ProcessEvent offset is outside the main module";
            return ctx;
        }

        ctx.world = safe_read<std::uintptr_t>(ctx.expected_gworld);
        if (!live_uobject(ctx.world))
        {
            ctx.stage = "world_unavailable";
            ctx.message = "UWorld::GetWorld returned null or invalid object";
            return ctx;
        }
        ctx.game_instance = safe_read<std::uintptr_t>(ctx.world + meccha_sdk::FieldOffsets::UWorld_OwningGameInstance);
        if (!live_uobject(ctx.game_instance))
        {
            ctx.stage = "world_unavailable";
            ctx.message = "UWorld::OwningGameInstance unavailable";
            return ctx;
        }

        const auto local_players = safe_read<meccha_sdk::TArray<std::uintptr_t>>(ctx.game_instance + meccha_sdk::FieldOffsets::UGameInstance_LocalPlayers);
        ctx.local_players_count = local_players.Num;
        if (!local_players.Data || local_players.Num <= 0 || local_players.Num > 8)
        {
            ctx.stage = "local_pawn_unavailable";
            ctx.message = "GameInstance.LocalPlayers is empty or invalid";
            return ctx;
        }
        ctx.local_player = safe_read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(local_players.Data));
        if (!live_uobject(ctx.local_player))
        {
            ctx.stage = "local_pawn_unavailable";
            ctx.message = "LocalPlayers[0] unavailable";
            return ctx;
        }
        ctx.controller = safe_read<std::uintptr_t>(ctx.local_player + meccha_sdk::FieldOffsets::UPlayer_PlayerController);
        if (!live_uobject(ctx.controller))
        {
            ctx.stage = "local_pawn_unavailable";
            ctx.message = "LocalPlayers[0].PlayerController unavailable";
            return ctx;
        }
        ctx.k2_get_pawn_function = ref.find_function(ctx.controller, "K2_GetPawn");
        if (!ctx.k2_get_pawn_function)
        {
            ctx.stage = "local_pawn_unavailable";
            ctx.message = "PlayerController.K2_GetPawn unavailable";
            return ctx;
        }
        meccha_sdk::Controller_K2_GetPawn pawn_params{};
        std::string process_failure{};
        if (!process_event(ctx.controller, ctx.k2_get_pawn_function, reinterpret_cast<std::uint8_t*>(&pawn_params), process_failure))
        {
            ctx.stage = "local_pawn_unavailable";
            ctx.message = "K2_GetPawn ProcessEvent failed: " + process_failure;
            return ctx;
        }
        ctx.pawn = reinterpret_cast<std::uintptr_t>(pawn_params.ReturnValue);
        if (!live_uobject(ctx.pawn))
        {
            ctx.stage = "local_pawn_unavailable";
            ctx.message = "K2_GetPawn returned null or invalid pawn";
            return ctx;
        }
        ctx.k2_get_actor_location_function = ref.find_function(ctx.pawn, "K2_GetActorLocation");
        if (ctx.k2_get_actor_location_function)
        {
            meccha_sdk::Actor_K2_GetActorLocation location_params{};
            std::string location_failure{};
            if (process_event(ctx.pawn, ctx.k2_get_actor_location_function, reinterpret_cast<std::uint8_t*>(&location_params), location_failure))
            {
                ctx.body_world_position = location_params.ReturnValue;
            }
        }
        ctx.component = safe_read<std::uintptr_t>(ctx.pawn + meccha_sdk::FieldOffsets::BP_FirstPersonCharacter_RuntimePaintable);
        const auto component_class = lower_copy(ref.class_name(ctx.component));
        if (!live_uobject(ctx.component) || !contains_text(component_class, "runtimepaint"))
        {
            ctx.stage = "paint_component_unavailable";
            ctx.message = "BP_FirstPersonCharacter.RuntimePaintable unavailable";
            return ctx;
        }
        ctx.relay_component = safe_read<std::uintptr_t>(ctx.controller + meccha_sdk::FieldOffsets::BP_PlayerController_RuntimePaintRelay);
        if (live_uobject(ctx.relay_component))
        {
            ctx.relay_stroke_batch_to_server_function = ref.find_function(ctx.relay_component, "RelayStrokeBatchToServer");
            ctx.relay_paint_to_server_function = ref.find_function(ctx.relay_component, "RelayPaintToServer");
        }
        ctx.export_function = ref.find_function(ctx.component, "ExportChannelToBytes");
        ctx.import_function = ref.find_function(ctx.component, "ImportChannelFromBytes");
        ctx.get_render_target_function = ref.find_function(ctx.component, "GetRenderTarget");
        ctx.server_paint_batch_function = ref.find_function(ctx.component, "ServerPaintBatch");
        ctx.server_send_stroke_batch_function = ref.find_function(ctx.component, "ServerSendStrokeBatch");
        ctx.server_send_paint_function = ref.find_function(ctx.component, "ServerSendPaint");
        ctx.server_paint_function = ref.find_function(ctx.component, "ServerPaint");
        ctx.paint_at_uv_with_brush_function = ref.find_function(ctx.component, "PaintAtUVWithBrush");
        ctx.ok = true;
        ctx.stage = "sdk_ready";
        ctx.message = "SDK context ready";
        return ctx;
    }

    struct SdkNativeFrontSampleResult
    {
        std::vector<FrontSample> samples{};
        std::string failure{};
        std::uintptr_t mesh{0};
        std::uintptr_t query{0};
        std::uintptr_t deproject_function{0};
        std::uintptr_t query_function{0};
        std::string deproject_schema{};
        std::string query_schema{};
        int viewport_width{0};
        int viewport_height{0};
        int deproject_calls{0};
        int deproject_ok{0};
        int deproject_return_false{0};
        int deproject_direction_invalid{0};
        int deproject_process_failed{0};
        int query_calls{0};
        int query_param_failed{0};
        int query_result_failed{0};
        int owner_matches{0};
        int owner_unknown_accepted{0};
        int owner_mismatch_rejected{0};
        int min_front_hits{2048};
        int target_front_hits{80000};
        int coarse_grid_x{96};
        int coarse_grid_y{72};
        int refine_grid_x{384};
        int refine_grid_y{320};
        int coarse_seeds{0};
        int refine_seeds{0};
        int hard_attempt_budget{0};
        int adaptive_passes{0};
        int adaptive_seeds{0};
        int adaptive_attempts{0};
        int adaptive_hits{0};
        double bbox_min_nx{1.0};
        double bbox_min_ny{1.0};
        double bbox_max_nx{0.0};
        double bbox_max_ny{0.0};
        int attempts{0};
        int query_success{0};
        int uv_hits{0};
        int duplicate_uv{0};
        int rejected{0};
        double first_screen_x{0.0};
        double first_screen_y{0.0};
        meccha_sdk::FVector first_ray_location{};
        meccha_sdk::FVector first_ray_direction{};
        double first_hit_u{0.0};
        double first_hit_v{0.0};
        meccha_sdk::FVector first_hit_world_position{};
        std::uintptr_t first_hit_actor{0};
        std::uintptr_t first_hit_component{0};
        std::string first_deproject_failure{};
        std::string first_query_failure{};
    };

    struct SdkFrontCaptureProbe
    {
        bool ok{false};
        std::string failure{"front_capture_unavailable"};
        int requested_samples{0};
        std::uintptr_t project_world_to_screen_function{0};
        std::uintptr_t scene_capture_class{0};
        std::uintptr_t create_render_target_function{0};
        std::uintptr_t read_render_target_raw_pixel_function{0};
        std::uintptr_t read_render_target_pixel_function{0};
        std::uintptr_t read_render_target_raw_function{0};
        std::uintptr_t read_render_target_function{0};
    };

    struct SdkFrontCaptureResult
    {
        bool ok{false};
        std::string failure{"front_capture_unavailable"};
        std::vector<FrontSample> samples{};
        int width{0};
        int height{0};
        std::uintptr_t render_target{0};
        std::uintptr_t capture_actor{0};
        std::uintptr_t capture_component{0};
        std::uintptr_t read_function{0};
        bool render_target_created{false};
        bool capture_actor_spawned{false};
        bool capture_component_found{false};
        bool texture_target_written{false};
        bool hide_component_called{false};
        bool capture_scene_called{false};
        double capture_fov{90.0};
        int viewport_width{0};
        int viewport_height{0};
        int requested_texture_width{0};
        int requested_texture_height{0};
        double viewport_aspect{1.0};
        double capture_aspect{1.0};
        double capture_scale_x{1.0};
        double capture_scale_y{1.0};
        std::string capture_resolution_source{"viewport"};
        std::uintptr_t camera_manager{0};
        bool camera_location_used{false};
        bool camera_rotation_used{false};
        bool camera_fov_used{false};
        std::string camera_location_source{"deproject_center"};
        std::string camera_rotation_source{"deproject_center_ray"};
        std::string camera_fov_source{"deproject_horizontal"};
        meccha_sdk::FVector capture_location{};
        meccha_sdk::FVector capture_direction{};
        int project_attempts{0};
        int project_success{0};
        int read_attempts{0};
        int read_success{0};
        int missing_color{0};
        double rgb_min{0.0};
        double rgb_max{0.0};
        double rgb_avg{0.0};
        double luma_range{0.0};
        int whiteish_samples{0};
        bool uniform{false};
        bool all_whiteish{false};
    };

    struct SdkUvGapFillStats
    {
        int candidates{0};
        int sent{0};
        int bounded{0};
        int edge_extended{0};
        int rejected_unbounded{0};
        int rejected_normal{0};
        int rejected_occupied{0};
        int direct_cells{0};
        int considered_cells{0};
        double coverage_before{0.0};
        double coverage_after{0.0};
    };

    auto sdk_find_object_named(Reflection& ref, const char* object_name) -> std::uintptr_t
    {
        std::uintptr_t found = 0;
        ref.for_each_object([&](std::uintptr_t object) {
            if (ref.object_name(object) == object_name)
            {
                found = object;
                return true;
            }
            return false;
        });
        return found;
    }

    auto sdk_probe_front_capture_backend(Reflection& ref,
                                         const SdkContext& ctx,
                                         const SdkNativeFrontSampleResult& samples) -> SdkFrontCaptureProbe
    {
        SdkFrontCaptureProbe out{};
        out.requested_samples = static_cast<int>(samples.samples.size());
        out.project_world_to_screen_function = ref.find_function(ctx.controller, "ProjectWorldLocationToScreen");
        out.scene_capture_class = ref.find_class("SceneCapture2D");
        out.create_render_target_function = sdk_find_object_named(ref, "CreateRenderTarget2D");
        out.read_render_target_raw_pixel_function = sdk_find_object_named(ref, "ReadRenderTargetRawPixel");
        out.read_render_target_pixel_function = sdk_find_object_named(ref, "ReadRenderTargetPixel");
        out.read_render_target_raw_function = sdk_find_object_named(ref, "ReadRenderTargetRaw");
        out.read_render_target_function = sdk_find_object_named(ref, "ReadRenderTarget");

        if (out.requested_samples <= 0)
        {
            out.failure = "front_capture_no_surface_samples";
            return out;
        }
        if (!out.project_world_to_screen_function)
        {
            out.failure = "front_capture_project_world_to_screen_unavailable";
            return out;
        }
        if (!out.scene_capture_class)
        {
            out.failure = "front_capture_scene_capture_class_unavailable";
            return out;
        }
        if (!out.create_render_target_function)
        {
            out.failure = "front_capture_create_render_target_unavailable";
            return out;
        }
        if (!out.read_render_target_raw_pixel_function && !out.read_render_target_pixel_function &&
            !out.read_render_target_raw_function && !out.read_render_target_function)
        {
            out.failure = "front_capture_read_render_target_unavailable";
            return out;
        }

        out.ok = true;
        out.failure = "ok";
        return out;
    }

    auto sdk_native_front_metadata(const SdkNativeFrontSampleResult& native_front) -> std::string
    {
        return ",\"front_sample_count\":" + std::to_string(native_front.samples.size()) +
               ",\"front_sample_source\":\"native_ue4ss_surface_samples\"" +
               ",\"front_mapping_required\":\"game_surface_trace_world_position\"" +
               ",\"front_native_min_surface_samples\":" + std::to_string(native_front.min_front_hits) +
               ",\"front_native_target_surface_samples\":" + std::to_string(native_front.target_front_hits) +
               ",\"front_native_coarse_grid_x\":" + std::to_string(native_front.coarse_grid_x) +
               ",\"front_native_coarse_grid_y\":" + std::to_string(native_front.coarse_grid_y) +
               ",\"front_native_refine_grid_x\":" + std::to_string(native_front.refine_grid_x) +
               ",\"front_native_refine_grid_y\":" + std::to_string(native_front.refine_grid_y) +
               ",\"front_native_coarse_seeds\":" + std::to_string(native_front.coarse_seeds) +
               ",\"front_native_refine_seeds\":" + std::to_string(native_front.refine_seeds) +
               ",\"front_native_hard_attempt_budget\":" + std::to_string(native_front.hard_attempt_budget) +
               ",\"front_native_adaptive_passes\":" + std::to_string(native_front.adaptive_passes) +
               ",\"front_native_adaptive_seeds\":" + std::to_string(native_front.adaptive_seeds) +
               ",\"front_native_adaptive_attempts\":" + std::to_string(native_front.adaptive_attempts) +
               ",\"front_native_adaptive_hits\":" + std::to_string(native_front.adaptive_hits) +
               ",\"front_native_bbox_min_nx\":" + std::to_string(native_front.bbox_min_nx) +
               ",\"front_native_bbox_min_ny\":" + std::to_string(native_front.bbox_min_ny) +
               ",\"front_native_bbox_max_nx\":" + std::to_string(native_front.bbox_max_nx) +
               ",\"front_native_bbox_max_ny\":" + std::to_string(native_front.bbox_max_ny) +
               ",\"front_native_query\":\"" + hex_address(native_front.query) + "\"" +
               ",\"front_native_mesh\":\"" + hex_address(native_front.mesh) + "\"" +
               ",\"front_native_deproject_function\":\"" + hex_address(native_front.deproject_function) + "\"" +
               ",\"front_native_query_function\":\"" + hex_address(native_front.query_function) + "\"" +
               ",\"front_native_viewport_width\":" + std::to_string(native_front.viewport_width) +
               ",\"front_native_viewport_height\":" + std::to_string(native_front.viewport_height) +
               ",\"front_native_deproject_calls\":" + std::to_string(native_front.deproject_calls) +
               ",\"front_native_deproject_ok\":" + std::to_string(native_front.deproject_ok) +
               ",\"front_native_query_calls\":" + std::to_string(native_front.query_calls) +
               ",\"front_native_owner_matches\":" + std::to_string(native_front.owner_matches) +
               ",\"front_native_query_success\":" + std::to_string(native_front.query_success) +
               ",\"front_native_uv_hits\":" + std::to_string(native_front.uv_hits) +
               ",\"front_native_duplicate_uv\":" + std::to_string(native_front.duplicate_uv) +
               ",\"front_native_rejected\":" + std::to_string(native_front.rejected) +
               ",\"front_native_first_hit_u\":" + std::to_string(native_front.first_hit_u) +
               ",\"front_native_first_hit_v\":" + std::to_string(native_front.first_hit_v) +
               ",\"front_native_first_hit_world_x\":" + std::to_string(native_front.first_hit_world_position.X) +
               ",\"front_native_first_hit_world_y\":" + std::to_string(native_front.first_hit_world_position.Y) +
               ",\"front_native_first_hit_world_z\":" + std::to_string(native_front.first_hit_world_position.Z) +
               ",\"front_native_first_hit_actor\":\"" + hex_address(native_front.first_hit_actor) + "\"" +
               ",\"front_native_first_hit_component\":\"" + hex_address(native_front.first_hit_component) + "\"" +
               ",\"front_native_first_deproject_failure\":\"" + json_escape(native_front.first_deproject_failure) + "\"" +
               ",\"front_native_first_query_failure\":\"" + json_escape(native_front.first_query_failure) + "\"" +
               ",\"front_native_failure\":\"" + json_escape(native_front.failure) + "\"";
    }

    auto sdk_front_capture_metadata(const SdkFrontCaptureProbe& capture) -> std::string
    {
        return ",\"capture_backend\":\"native_scene_capture_required\"" +
               std::string(",\"srgb\":true") +
               ",\"front_capture_requested_samples\":" + std::to_string(capture.requested_samples) +
               ",\"front_capture_project_world_to_screen_function\":\"" + hex_address(capture.project_world_to_screen_function) + "\"" +
               ",\"front_capture_scene_capture_class\":\"" + hex_address(capture.scene_capture_class) + "\"" +
               ",\"front_capture_create_render_target_function\":\"" + hex_address(capture.create_render_target_function) + "\"" +
               ",\"front_capture_read_raw_pixel_function\":\"" + hex_address(capture.read_render_target_raw_pixel_function) + "\"" +
               ",\"front_capture_read_pixel_function\":\"" + hex_address(capture.read_render_target_pixel_function) + "\"" +
               ",\"front_capture_read_raw_function\":\"" + hex_address(capture.read_render_target_raw_function) + "\"" +
               ",\"front_capture_read_function\":\"" + hex_address(capture.read_render_target_function) + "\"" +
               ",\"front_capture_failure\":\"" + json_escape(capture.failure) + "\"";
    }

    auto sdk_object_is_or_belongs_to(Reflection& ref, std::uintptr_t object, std::uintptr_t target) -> bool
    {
        if (!live_uobject(object) || !live_uobject(target))
        {
            return false;
        }
        if (object == target)
        {
            return true;
        }
        for (auto current = object; live_uobject(current); current = safe_read<std::uintptr_t>(current + OffOuter))
        {
            if (current == target)
            {
                return true;
            }
        }
        const auto owner = read_object_property_by_names(ref, object, {"OwnerPrivate", "Owner"});
        if (owner == target)
        {
            return true;
        }
        for (auto current = owner; live_uobject(current); current = safe_read<std::uintptr_t>(current + OffOuter))
        {
            if (current == target)
            {
                return true;
            }
        }
        return false;
    }

    auto sdk_find_front_mesh(Reflection& ref, const SdkContext& ctx) -> std::uintptr_t
    {
        if (const auto mesh = read_object_property_by_names(ref,
                                                            ctx.pawn,
                                                            {"Mesh",
                                                             "FirstPersonMesh",
                                                             "BodyMesh",
                                                             "CharacterMesh",
                                                             "SkeletalMeshComponent",
                                                             "TargetMeshComponent"}))
        {
            return mesh;
        }
        std::uintptr_t found = 0;
        ref.for_each_object([&](std::uintptr_t object) {
            if (!live_uobject(object))
            {
                return false;
            }
            const auto cls = lower_copy(ref.class_name(object));
            if (!contains_text(cls, "meshcomponent"))
            {
                return false;
            }
            if (sdk_object_is_or_belongs_to(ref, object, ctx.pawn))
            {
                found = object;
                return true;
            }
            return false;
        });
        return found;
    }

    auto sdk_find_screen_space_brush_query(Reflection& ref, const SdkContext& ctx) -> std::uintptr_t
    {
        std::uintptr_t fallback = 0;
        std::uintptr_t owned = 0;
        ref.for_each_object([&](std::uintptr_t object) {
            if (!live_uobject(object))
            {
                return false;
            }
            const auto cls = lower_copy(ref.class_name(object));
            const auto name = lower_copy(ref.object_name(object));
            if (!contains_text(cls, "screenspacebrushquery") && !contains_text(name, "screenspacebrushquery"))
            {
                return false;
            }
            if (!ref.find_function(object, "QueryFromWorldRay"))
            {
                return false;
            }
            if (!fallback)
            {
                fallback = object;
            }
            if (sdk_object_is_or_belongs_to(ref, object, ctx.pawn) || sdk_object_is_or_belongs_to(ref, object, ctx.controller))
            {
                owned = object;
                return true;
            }
            return false;
        });
        return owned ? owned : fallback;
    }

    auto sdk_configure_screen_space_brush_query(Reflection& ref, std::uintptr_t query, std::uintptr_t pawn, std::uintptr_t mesh) -> bool
    {
        if (!live_uobject(query) || !live_uobject(pawn))
        {
            return false;
        }
        sdk_call_no_params(ref, query, "ResetFilter");
        sdk_call_no_params(ref, query, "ClearTargetComponents");
        sdk_call_no_params(ref, query, "ClearTargetActors");
        sdk_call_no_params(ref, query, "ClearNoCollisionMeshTargets");
        sdk_call_no_params(ref, query, "ClearIgnoreActors");
        sdk_call_single_number(ref, query, "SetUVChannel", 0.0);
        sdk_call_single_number(ref, query, "SetMaxTraceDistance", 12000.0);
        sdk_call_single_bool(ref, query, "SetTraceComplex", true);
        sdk_call_single_bool(ref, query, "SetAllowNoCollisionMesh", true);
        const bool actor_ok = sdk_call_object_param(ref, query, "AddTargetActor", pawn);
        const bool component_ok = mesh && sdk_call_object_param(ref, query, "AddTargetComponent", mesh);
        const bool no_collision_ok = mesh && sdk_call_object_param(ref, query, "AddNoCollisionMeshTarget", mesh);
        return ref.find_function(query, "QueryFromWorldRay") && (actor_ok || component_ok || no_collision_ok);
    }

    auto sdk_get_viewport_info(Reflection& ref, const SdkContext& ctx) -> SdkViewportInfo
    {
        SdkViewportInfo out{};
        const auto function = ref.find_function(ctx.controller, "GetViewportSize");
        if (!function)
        {
            return out;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 1024)
        {
            return out;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        std::string failure{};
        if (!process_event(ctx.controller, function, params.data(), failure))
        {
            return out;
        }
        std::vector<int> numeric_values{};
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue")
            {
                continue;
            }
            const int value = static_cast<int>(sdk_read_number(ref, prop, params.data()));
            if (value <= 0)
            {
                continue;
            }
            if (contains_text(name, "sizex") || contains_text(name, "width") || name == "x")
            {
                out.width = value;
            }
            else if (contains_text(name, "sizey") || contains_text(name, "height") || name == "y")
            {
                out.height = value;
            }
            numeric_values.push_back(value);
        }
        if ((out.width <= 0 || out.height <= 0) && numeric_values.size() >= 2)
        {
            out.width = numeric_values[0];
            out.height = numeric_values[1];
        }
        return out;
    }

    auto sdk_deproject_screen_position(Reflection& ref, const SdkContext& ctx, double screen_x, double screen_y) -> SdkDeprojectRay
    {
        SdkDeprojectRay out{};
        const auto function = ref.find_function(ctx.controller, "DeprojectScreenPositionToWorld");
        if (!function)
        {
            out.failure = "deproject_function_unavailable";
            return out;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 2048)
        {
            out.failure = "deproject_params_size_invalid";
            return out;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        int numeric_index = 0;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue")
            {
                continue;
            }
            if (strict_vector_struct_type(ref, prop, {"X", "Y", "Z"}, 12))
            {
                continue;
            }
            if (contains_text(name, "screenx") || contains_text(name, "screen_x") || name == "x" || numeric_index == 0)
            {
                write_number(ref, prop, params.data(), screen_x);
                ++numeric_index;
            }
            else if (contains_text(name, "screeny") || contains_text(name, "screen_y") || name == "y" || numeric_index == 1)
            {
                write_number(ref, prop, params.data(), screen_y);
                ++numeric_index;
            }
        }
        std::string failure{};
        if (!process_event(ctx.controller, function, params.data(), failure))
        {
            out.failure = "deproject_process_event_failed:" + failure;
            return out;
        }
        out.ok = read_return_bool(ref, function, params.data());
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue")
            {
                continue;
            }
            if (!strict_vector_struct_type(ref, prop, {"X", "Y", "Z"}, 12))
            {
                continue;
            }
            if (contains_text(name, "worldlocation") || contains_text(name, "world_location") || contains_text(name, "location"))
            {
                sdk_read_vector3(ref, prop, params.data(), out.location);
            }
            else if (contains_text(name, "worlddirection") || contains_text(name, "world_direction") || contains_text(name, "direction"))
            {
                meccha_sdk::FVector direction{};
                if (sdk_read_vector3(ref, prop, params.data(), direction))
                {
                    out.direction = sdk_vec_normalize(direction);
                }
            }
        }
        if (!out.ok)
        {
            out.failure = "deproject_return_false";
        }
        else if (sdk_vec_len(out.direction) < 0.01)
        {
            out.ok = false;
            out.failure = "deproject_direction_invalid";
        }
        return out;
    }

    auto sdk_decode_brush_query_result(Reflection& ref, std::uintptr_t function, std::uint8_t* params) -> SdkBrushQueryHit
    {
        SdkBrushQueryHit out{};
        std::uintptr_t return_prop = 0;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            if (ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)) == "ReturnValue")
            {
                return_prop = prop;
                break;
            }
        }
        if (!return_prop)
        {
            out.failure = "brush_query_return_struct_unavailable";
            return out;
        }
        const auto offset = prop_offset(return_prop);
        const auto structure = struct_type(ref, return_prop, {"bSuccess", "HitUV", "WorldPosition", "HitComponent", "HitActor"});
        if (offset < 0 || !structure)
        {
            out.failure = "brush_query_return_struct_invalid";
            return out;
        }
        auto* base = params + offset;
        out.params_ok = true;
        bool has_success_property = false;
        if (const auto p = find_property_any(ref, structure, {"bSuccess"}))
        {
            has_success_property = true;
            out.success = sdk_read_bool(p, base);
        }
        if (const auto p = find_property_any(ref, structure, {"bHasUV"}))
        {
            out.has_uv = sdk_read_bool(p, base);
        }
        if (const auto p = find_property_any(ref, structure, {"HitUV"}))
        {
            out.has_uv = sdk_read_vector2(ref, p, base, out.u, out.v) || out.has_uv;
        }
        if (const auto p = find_property_any(ref, structure, {"WorldPosition", "HitWorldPosition"}))
        {
            sdk_read_vector3(ref, p, base, out.world_position);
        }
        if (const auto p = find_property_any(ref, structure, {"WorldNormal", "HitNormal"}))
        {
            sdk_read_vector3(ref, p, base, out.normal);
        }
        if (const auto p = find_property_any(ref, structure, {"HitActor", "Actor"}))
        {
            out.actor = sdk_read_object(p, base);
        }
        if (const auto p = find_property_any(ref, structure, {"HitComponent", "Component"}))
        {
            out.component = sdk_read_object(p, base);
        }
        if (!has_success_property)
        {
            out.success = out.has_uv;
        }
        if (!out.success && out.failure.empty())
        {
            out.failure = "brush_query_result_unsuccessful";
        }
        return out;
    }

    auto sdk_query_brush_from_world_ray(Reflection& ref,
                                        std::uintptr_t query,
                                        const meccha_sdk::FVector& origin,
                                        const meccha_sdk::FVector& direction) -> SdkBrushQueryHit
    {
        SdkBrushQueryHit out{};
        const auto function = ref.find_function(query, "QueryFromWorldRay");
        if (!function)
        {
            out.failure = "query_from_world_ray_unavailable";
            return out;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 4096)
        {
            out.failure = "query_from_world_ray_params_size_invalid";
            return out;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        bool wrote_origin = false;
        bool wrote_direction = false;
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue")
            {
                continue;
            }
            if (contains_text(name, "origin"))
            {
                wrote_origin = sdk_write_vector3(ref, prop, params.data(), origin) || wrote_origin;
            }
            else if (contains_text(name, "direction"))
            {
                wrote_direction = sdk_write_vector3(ref, prop, params.data(), sdk_vec_normalize(direction)) || wrote_direction;
            }
        }
        if (!wrote_origin || !wrote_direction)
        {
            out.failure = !wrote_origin ? "brush_query_origin_param_unresolved" : "brush_query_direction_param_unresolved";
            return out;
        }
        std::string failure{};
        if (!process_event(query, function, params.data(), failure))
        {
            out.failure = "brush_query_process_event_failed:" + failure;
            return out;
        }
        return sdk_decode_brush_query_result(ref, function, params.data());
    }

    auto sdk_collect_native_front_samples(Reflection& ref,
                                          const SdkContext& ctx,
                                          const std::vector<FrontSample>& color_source) -> SdkNativeFrontSampleResult
    {
        SdkNativeFrontSampleResult result{};
        result.mesh = sdk_find_front_mesh(ref, ctx);
        result.query = sdk_find_screen_space_brush_query(ref, ctx);
        result.deproject_function = ref.find_function(ctx.controller, "DeprojectScreenPositionToWorld");
        result.query_function = result.query ? ref.find_function(result.query, "QueryFromWorldRay") : 0;
        result.deproject_schema = function_param_schema(ref, result.deproject_function);
        result.query_schema = function_param_schema(ref, result.query_function);
        if (!result.query)
        {
            result.failure = "screen_space_brush_query_unavailable";
            return result;
        }
        if (!sdk_configure_screen_space_brush_query(ref, result.query, ctx.pawn, result.mesh))
        {
            result.failure = "screen_space_brush_query_config_failed";
            return result;
        }
        const auto viewport = sdk_get_viewport_info(ref, ctx);
        result.viewport_width = viewport.width;
        result.viewport_height = viewport.height;
        if (viewport.width <= 0 || viewport.height <= 0)
        {
            result.failure = "viewport_unavailable";
            return result;
        }

        const int target_samples = std::max(result.min_front_hits, result.target_front_hits);
        const int hard_attempts = 220000;
        result.hard_attempt_budget = hard_attempts;
        std::vector<std::uint64_t> unique_texels{};
        unique_texels.reserve(static_cast<std::size_t>(target_samples));
        result.samples.reserve(static_cast<std::size_t>(target_samples));

        auto cell_jitter = [](int x, int y, int salt) -> double {
            std::uint32_t n = static_cast<std::uint32_t>(x) * 0x9E3779B1u ^
                              static_cast<std::uint32_t>(y) * 0x85EBCA77u ^
                              static_cast<std::uint32_t>(salt) * 0xC2B2AE3Du;
            n ^= n >> 16;
            n *= 0x7FEB352Du;
            n ^= n >> 15;
            n *= 0x846CA68Bu;
            n ^= n >> 16;
            return static_cast<double>(n & 0xFFFFu) / 65535.0;
        };

        auto record_bbox = [&](double nx, double ny) {
            result.bbox_min_nx = std::min(result.bbox_min_nx, nx);
            result.bbox_min_ny = std::min(result.bbox_min_ny, ny);
            result.bbox_max_nx = std::max(result.bbox_max_nx, nx);
            result.bbox_max_ny = std::max(result.bbox_max_ny, ny);
        };

        auto try_sample = [&](double nx, double ny, bool refined) -> bool {
            if (result.deproject_calls >= hard_attempts || static_cast<int>(result.samples.size()) >= target_samples)
            {
                return false;
            }
            nx = clamp01(nx);
            ny = clamp01(ny);
            const auto screen_x = nx * static_cast<double>(viewport.width);
            const auto screen_y = ny * static_cast<double>(viewport.height);
            const auto ray = sdk_deproject_screen_position(ref, ctx, screen_x, screen_y);
            ++result.deproject_calls;
            if (result.deproject_calls == 1)
            {
                result.first_screen_x = screen_x;
                result.first_screen_y = screen_y;
                result.first_ray_location = ray.location;
                result.first_ray_direction = ray.direction;
            }
            if (!ray.ok)
            {
                if (result.failure.empty())
                {
                    result.failure = ray.failure;
                }
                if (result.first_deproject_failure.empty())
                {
                    result.first_deproject_failure = ray.failure;
                }
                if (contains_text(ray.failure, "return_false"))
                {
                    ++result.deproject_return_false;
                }
                else if (contains_text(ray.failure, "direction_invalid"))
                {
                    ++result.deproject_direction_invalid;
                }
                else if (contains_text(ray.failure, "process_event"))
                {
                    ++result.deproject_process_failed;
                }
                ++result.rejected;
                return false;
            }
            ++result.deproject_ok;
            ++result.attempts;
            const auto hit = sdk_query_brush_from_world_ray(ref, result.query, ray.location, ray.direction);
            ++result.query_calls;
            if (!hit.params_ok)
            {
                if (result.failure.empty())
                {
                    result.failure = hit.failure;
                }
                if (result.first_query_failure.empty())
                {
                    result.first_query_failure = hit.failure;
                }
                ++result.query_param_failed;
                ++result.rejected;
                return false;
            }
            if (!hit.success || !hit.has_uv || !std::isfinite(hit.u) || !std::isfinite(hit.v) ||
                !std::isfinite(hit.world_position.X) || !std::isfinite(hit.world_position.Y) || !std::isfinite(hit.world_position.Z))
            {
                if (result.samples.empty() && result.first_query_failure.empty())
                {
                    result.first_query_failure = hit.failure.empty() ? "brush_query_no_hit" : hit.failure;
                }
                ++result.query_result_failed;
                ++result.rejected;
                return false;
            }
            ++result.query_success;
            if (result.query_success == 1)
            {
                result.first_hit_u = hit.u;
                result.first_hit_v = hit.v;
                result.first_hit_world_position = hit.world_position;
                result.first_hit_actor = hit.actor;
                result.first_hit_component = hit.component;
            }
            const bool owner_hit = (hit.component && hit.component == result.mesh) ||
                                   sdk_object_is_or_belongs_to(ref, hit.actor, ctx.pawn) ||
                                   sdk_object_is_or_belongs_to(ref, hit.component, ctx.pawn);
            if (!owner_hit)
            {
                if (result.samples.empty() && result.first_query_failure.empty())
                {
                    result.first_query_failure = (!hit.actor && !hit.component)
                        ? "brush_query_owner_unavailable"
                        : "brush_query_owner_mismatch";
                }
                if (!hit.actor && !hit.component)
                {
                    ++result.owner_unknown_accepted;
                }
                else
                {
                    ++result.owner_mismatch_rejected;
                }
                ++result.rejected;
                return false;
            }
            ++result.owner_matches;
            ++result.uv_hits;
            const auto ux = static_cast<std::uint64_t>(std::max(0, std::min(4095, static_cast<int>(clamp01(hit.u) * 4096.0))));
            const auto uy = static_cast<std::uint64_t>(std::max(0, std::min(4095, static_cast<int>(clamp01(hit.v) * 4096.0))));
            const auto key = (uy << 32) | ux;
            if (std::find(unique_texels.begin(), unique_texels.end(), key) != unique_texels.end())
            {
                ++result.duplicate_uv;
                return false;
            }
            unique_texels.push_back(key);

            FrontSample sample{};
            sample.u = clamp01(hit.u);
            sample.v = clamp01(hit.v);
            sample.r = 0.34;
            sample.g = 0.36;
            sample.b = 0.32;
            sample.roughness = 0.65;
            sample.radius = 0.0025;
            sample.screen_nx = nx;
            sample.screen_ny = ny;
            sample.has_world_position = true;
            sample.world_position = hit.world_position;
            sample.normal = sdk_vec_normalize(hit.normal);
            result.samples.push_back(sample);
            record_bbox(nx, ny);
            if (refined)
            {
                ++result.refine_seeds;
            }
            else
            {
                ++result.coarse_seeds;
            }
            return true;
        };

        for (int y = 0; y < result.coarse_grid_y && static_cast<int>(result.samples.size()) < target_samples; ++y)
        {
            for (int x = 0; x < result.coarse_grid_x && static_cast<int>(result.samples.size()) < target_samples; ++x)
            {
                const auto jx = (cell_jitter(x, y, 11) - 0.5) * 0.72;
                const auto jy = (cell_jitter(x, y, 23) - 0.5) * 0.72;
                const auto nx = 0.24 + ((static_cast<double>(x) + 0.5 + jx) / static_cast<double>(result.coarse_grid_x)) * 0.52;
                const auto ny = 0.08 + ((static_cast<double>(y) + 0.5 + jy) / static_cast<double>(result.coarse_grid_y)) * 0.84;
                try_sample(nx, ny, false);
            }
        }

        const bool have_bbox = result.bbox_max_nx >= result.bbox_min_nx && result.bbox_max_ny >= result.bbox_min_ny;
        const auto span_x = have_bbox ? std::max(0.02, result.bbox_max_nx - result.bbox_min_nx) : 0.52;
        const auto span_y = have_bbox ? std::max(0.02, result.bbox_max_ny - result.bbox_min_ny) : 0.84;
        const auto query_min_nx = have_bbox ? clamp01(result.bbox_min_nx - span_x * 0.35) : 0.24;
        const auto query_max_nx = have_bbox ? clamp01(result.bbox_max_nx + span_x * 0.35) : 0.76;
        const auto query_min_ny = have_bbox ? clamp01(result.bbox_min_ny - span_y * 0.18) : 0.08;
        const auto query_max_ny = have_bbox ? clamp01(result.bbox_max_ny + span_y * 0.55) : 0.92;

        for (int y = 0; y < result.refine_grid_y && static_cast<int>(result.samples.size()) < target_samples; ++y)
        {
            for (int x = 0; x < result.refine_grid_x && static_cast<int>(result.samples.size()) < target_samples; ++x)
            {
                const auto jx = (cell_jitter(x, y, 37) - 0.5) * 0.84;
                const auto jy = (cell_jitter(x, y, 41) - 0.5) * 0.84;
                const auto local_nx = (static_cast<double>(x) + 0.5 + jx) / static_cast<double>(result.refine_grid_x);
                const auto local_ny = (static_cast<double>(y) + 0.5 + jy) / static_cast<double>(result.refine_grid_y);
                const auto nx = query_min_nx + local_nx * (query_max_nx - query_min_nx);
                const auto ny = query_min_ny + local_ny * (query_max_ny - query_min_ny);
                try_sample(nx, ny, true);
            }
        }

        const double adaptive_base_dx = std::max(0.00045, (query_max_nx - query_min_nx) / static_cast<double>(std::max(64, result.refine_grid_x)));
        const double adaptive_base_dy = std::max(0.00045, (query_max_ny - query_min_ny) / static_cast<double>(std::max(64, result.refine_grid_y)));
        const double directions[][2] = {
            {1.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0}, {0.0, -1.0},
            {0.7071, 0.7071}, {-0.7071, 0.7071}, {0.7071, -0.7071}, {-0.7071, -0.7071},
            {1.0, 0.45}, {-1.0, 0.45}, {1.0, -0.45}, {-1.0, -0.45},
        };
        for (int pass = 0;
             pass < 8 && result.deproject_calls < hard_attempts && static_cast<int>(result.samples.size()) < target_samples;
             ++pass)
        {
            const auto seeds = result.samples;
            if (seeds.empty())
            {
                break;
            }
            ++result.adaptive_passes;
            result.adaptive_seeds += static_cast<int>(seeds.size());
            const auto ring = static_cast<double>(pass + 1);
            const auto pass_dx = adaptive_base_dx * ring;
            const auto pass_dy = adaptive_base_dy * ring;
            for (std::size_t i = 0;
                 i < seeds.size() && result.deproject_calls < hard_attempts && static_cast<int>(result.samples.size()) < target_samples;
                 ++i)
            {
                const auto& seed = seeds[i];
                for (int d = 0;
                     d < static_cast<int>(sizeof(directions) / sizeof(directions[0])) &&
                     result.deproject_calls < hard_attempts &&
                     static_cast<int>(result.samples.size()) < target_samples;
                     ++d)
                {
                    const auto jitter_scale = 0.82 + cell_jitter(static_cast<int>(i), pass, d) * 0.36;
                    const auto nx = seed.screen_nx + directions[d][0] * pass_dx * jitter_scale;
                    const auto ny = seed.screen_ny + directions[d][1] * pass_dy * jitter_scale;
                    const auto before = result.samples.size();
                    ++result.adaptive_attempts;
                    if (try_sample(nx, ny, true) && result.samples.size() > before)
                    {
                        ++result.adaptive_hits;
                    }
                }
            }
        }

        if (result.samples.empty() && result.failure.empty())
        {
            result.failure = "native_front_surface_samples_empty";
        }
        return result;
    }

    auto sdk_export_channel_bytes(Reflection& ref, const SdkContext& ctx, int channel) -> ChannelBuffer
    {
        (void)ref;
        ChannelBuffer out{};
        out.channel = channel;
        if (!ctx.export_function)
        {
            out.failure = "export_unavailable";
            return out;
        }
        meccha_sdk::RuntimePaintableComponent_ExportChannelToBytes params{};
        params.Channel = static_cast<meccha_sdk::EPaintChannel>(channel);
        std::string failure{};
        if (!process_event(ctx.component, ctx.export_function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            out.failure = "export_process_event_failed:" + failure;
            return out;
        }
        if (!params.ReturnValue)
        {
            out.failure = "export_return_false";
            return out;
        }
        if (!params.OutData.Data || params.OutData.Num <= 0 || params.OutData.Num > 128 * 1024 * 1024 || params.OutData.Max < params.OutData.Num)
        {
            out.failure = "export_array_invalid";
            return out;
        }
        out.bytes.resize(static_cast<std::size_t>(params.OutData.Num));
        if (!safe_copy(out.bytes.data(), params.OutData.Data, out.bytes.size()))
        {
            out.failure = "export_array_copy_failed";
            out.bytes.clear();
            return out;
        }
        out.hash = hash_bytes(out.bytes);
        const auto [width, height] = infer_channel_dimensions(out.bytes.size());
        out.width = width;
        out.height = height;
        out.bytes_per_pixel = (width > 0 && height > 0 && out.bytes.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) ? 4 : 1;
        out.ok = true;
        return out;
    }

    auto sdk_import_channel_bytes(const SdkContext& ctx, int channel, const std::vector<std::uint8_t>& bytes, std::string& failure) -> bool
    {
        failure.clear();
        if (!ctx.import_function)
        {
            failure = "import_unavailable";
            return false;
        }
        if (bytes.empty() || bytes.size() > 128 * 1024 * 1024)
        {
            failure = "import_bytes_invalid";
            return false;
        }
        meccha_sdk::RuntimePaintableComponent_ImportChannelFromBytes params{};
        params.Channel = static_cast<meccha_sdk::EPaintChannel>(channel);
        params.Data.Data = const_cast<std::uint8_t*>(bytes.data());
        params.Data.Num = static_cast<std::int32_t>(bytes.size());
        params.Data.Max = static_cast<std::int32_t>(bytes.size());
        std::string process_failure{};
        if (!process_event(ctx.component, ctx.import_function, reinterpret_cast<std::uint8_t*>(&params), process_failure))
        {
            failure = "import_process_event_failed:" + process_failure;
            return false;
        }
        if (!params.ReturnValue)
        {
            failure = "import_return_false";
            return false;
        }
        return true;
    }

    auto sdk_get_render_target(const SdkContext& ctx, int channel) -> std::uintptr_t
    {
        if (!ctx.get_render_target_function)
        {
            return 0;
        }
        meccha_sdk::RuntimePaintableComponent_GetRenderTarget params{};
        params.Channel = static_cast<meccha_sdk::EPaintChannel>(channel);
        std::string failure{};
        if (!process_event(ctx.component, ctx.get_render_target_function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            return 0;
        }
        return reinterpret_cast<std::uintptr_t>(params.ReturnValue);
    }

    auto sdk_channel_metadata(const char* prefix, const ChannelBuffer& channel) -> std::string
    {
        std::string name(prefix);
        return ",\"" + name + "_ok\":" + json_bool(channel.ok) +
               ",\"" + name + "_channel\":" + std::to_string(channel.channel) +
               ",\"" + name + "_width\":" + std::to_string(channel.width) +
               ",\"" + name + "_height\":" + std::to_string(channel.height) +
               ",\"" + name + "_bytes_per_pixel\":" + std::to_string(channel.bytes_per_pixel) +
               ",\"" + name + "_bytes\":" + std::to_string(channel.bytes.size()) +
               ",\"" + name + "_hash\":\"" + std::to_string(channel.hash) + "\"" +
               ",\"" + name + "_first_bytes\":\"" + first_bytes_hex(channel.bytes) + "\"" +
               ",\"" + name + "_failure\":\"" + json_escape(channel.failure) + "\"";
    }

    struct SdkReplicatedStats
    {
        int requested{0};
        int server_sent{0};
        int server_failed{0};
        int batch_calls{0};
        int single_calls{0};
        int client_mirror_sent{0};
        int client_mirror_failed{0};
        std::string server_rpc{"<none>"};
        std::string client_mirror_rpc{"<none>"};
        std::string first_failure{};
    };

    auto sdk_has_replicated_api(const SdkContext& ctx) -> bool
    {
        return ctx.server_paint_batch_function != 0;
    }

    auto sdk_copy_current_brush(const SdkContext& ctx, double radius) -> meccha_sdk::FRuntimeBrushSettings
    {
        meccha_sdk::FRuntimeBrushSettings brush{};
        safe_copy(&brush,
                  reinterpret_cast<void*>(ctx.component + meccha_sdk::FieldOffsets::RuntimePaintable_CurrentBrushSettings),
                  sizeof(brush));
        brush.Radius = static_cast<float>(std::max(0.001, std::min(0.12, radius)));
        brush.Hardness = 1.0f;
        brush.Opacity = 1.0f;
        brush.Spacing = std::min(brush.Spacing > 0.0f ? brush.Spacing : 0.25f, 0.25f);
        brush.Falloff = meccha_sdk::EBrushFalloff::Spherical;
        brush.BlendMode = meccha_sdk::EPaintBlendMode::Normal;
        brush.Rotation = 0.0f;
        return brush;
    }

    auto sdk_make_channel(double r,
                          double g,
                          double b,
                          double metallic,
                          double roughness,
                          meccha_sdk::EPaintChannelApplyMode apply_mode) -> meccha_sdk::FPaintChannelData
    {
        meccha_sdk::FPaintChannelData data{};
        data.AlbedoColor.R = static_cast<float>(clamp01(r));
        data.AlbedoColor.G = static_cast<float>(clamp01(g));
        data.AlbedoColor.B = static_cast<float>(clamp01(b));
        data.AlbedoColor.A = 1.0f;
        data.Metallic = static_cast<float>(clamp01(metallic));
        data.Roughness = static_cast<float>(clamp01(roughness));
        data.Height = 0.0f;
        data.ApplyMode = apply_mode;
        return data;
    }

    auto sdk_make_stroke(double u,
                         double v,
                         const meccha_sdk::FPaintChannelData& channel,
                         const meccha_sdk::FRuntimeBrushSettings& brush,
                         meccha_sdk::EPaintChannel target_channel,
                         const meccha_sdk::FVector& world_position) -> meccha_sdk::FPaintStroke
    {
        meccha_sdk::FPaintStroke stroke{};
        stroke.Uv.X = clamp01(u);
        stroke.Uv.Y = clamp01(v);
        stroke.WorldPosition = world_position;
        stroke.bHasWorldPosition = true;
        stroke.bHasLocalPosition = false;
        stroke.bHasSkeletalTriangleAnchor = false;
        stroke.BrushSettings = brush;
        stroke.ChannelData = channel;
        stroke.TargetChannel = target_channel;
        stroke.EffectiveBrushWorldRadius = brush.Radius;
        stroke.EffectiveSubdivisionLevel = 0;
        stroke.EffectiveSubdivisionPixelSize = 1.0f;
        stroke.EffectiveTemplateResolution = 0;
        stroke.EffectiveMaxGeneratedBrushTriangles = 0;
        stroke.EffectiveGutterExpandPixels = 0;
        return stroke;
    }

    auto sdk_make_uv_stroke(double u,
                            double v,
                            const meccha_sdk::FPaintChannelData& channel,
                            const meccha_sdk::FRuntimeBrushSettings& brush,
                            meccha_sdk::EPaintChannel target_channel) -> meccha_sdk::FPaintStroke
    {
        auto stroke = sdk_make_stroke(u, v, channel, brush, target_channel, {});
        stroke.bHasWorldPosition = false;
        stroke.WorldPosition = {};
        return stroke;
    }

    auto sdk_call_server_paint_batch(const SdkContext& ctx,
                                     const std::vector<meccha_sdk::FPaintStroke>& strokes,
                                     std::size_t offset,
                                     std::size_t count,
                                     std::string& failure) -> bool
    {
        if (!ctx.server_paint_batch_function || count == 0)
        {
            failure = "server_paint_batch_unavailable";
            return false;
        }
        if (!live_uobject(ctx.component))
        {
            failure = "paint_component_unavailable";
            return false;
        }
        meccha_sdk::RuntimePaintableComponent_ServerPaintBatch params{};
        params.Batch.Strokes.Data = const_cast<meccha_sdk::FPaintStroke*>(strokes.data() + offset);
        params.Batch.Strokes.Num = static_cast<std::int32_t>(count);
        params.Batch.Strokes.Max = static_cast<std::int32_t>(count);
        if (!process_event(ctx.component, ctx.server_paint_batch_function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            return false;
        }
        return true;
    }

    auto sdk_call_client_mirror(const SdkContext& ctx,
                                const meccha_sdk::FPaintStroke& stroke,
                                std::string& failure) -> bool
    {
        if (!ctx.paint_at_uv_with_brush_function)
        {
            failure = "PaintAtUVWithBrush_unavailable";
            return false;
        }
        if (!live_uobject(ctx.component))
        {
            failure = "paint_component_unavailable";
            return false;
        }
        meccha_sdk::RuntimePaintableComponent_PaintAtUVWithBrush params{};
        params.Uv = stroke.Uv;
        params.ChannelData = stroke.ChannelData;
        params.BrushSettings = stroke.BrushSettings;
        params.Channel = stroke.TargetChannel;
        if (!process_event(ctx.component, ctx.paint_at_uv_with_brush_function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            return false;
        }
        return true;
    }

    auto sdk_call_relay_batch_rpc(const SdkContext& ctx,
                                  std::uintptr_t function,
                                  const std::vector<meccha_sdk::FPaintStroke>& strokes,
                                  std::size_t offset,
                                  std::size_t count,
                                  std::string& failure) -> bool
    {
        if (!function || count == 0)
        {
            failure = "relay_batch_rpc_unavailable";
            return false;
        }
        if (!live_uobject(ctx.relay_component) || !live_uobject(ctx.component))
        {
            failure = "relay_or_paint_component_unavailable";
            return false;
        }
        meccha_sdk::RuntimePaintRelayComponent_RelayStrokeBatchToServer params{};
        params.PaintComponent = reinterpret_cast<void*>(ctx.component);
        params.Batch.Strokes.Data = const_cast<meccha_sdk::FPaintStroke*>(strokes.data() + offset);
        params.Batch.Strokes.Num = static_cast<std::int32_t>(count);
        params.Batch.Strokes.Max = static_cast<std::int32_t>(count);
        if (!process_event(ctx.relay_component, function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            return false;
        }
        return true;
    }

    auto sdk_call_relay_single_rpc(const SdkContext& ctx,
                                   std::uintptr_t function,
                                   const meccha_sdk::FPaintStroke& stroke,
                                   std::string& failure) -> bool
    {
        if (!function)
        {
            failure = "relay_single_rpc_unavailable";
            return false;
        }
        if (!live_uobject(ctx.relay_component) || !live_uobject(ctx.component))
        {
            failure = "relay_or_paint_component_unavailable";
            return false;
        }
        meccha_sdk::RuntimePaintRelayComponent_RelayPaintToServer params{};
        params.PaintComponent = reinterpret_cast<void*>(ctx.component);
        params.Stroke = stroke;
        if (!process_event(ctx.relay_component, function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            return false;
        }
        return true;
    }

    auto sdk_dispatch_replicated_strokes(const SdkContext& ctx,
                                         const std::vector<meccha_sdk::FPaintStroke>& strokes,
                                         SdkReplicatedStats& stats) -> bool
    {
        stats.requested = static_cast<int>(strokes.size());
        if (strokes.empty())
        {
            return true;
        }

        const int raw_limit = safe_read<int>(ctx.component + meccha_sdk::FieldOffsets::RuntimePaintable_MaxReplicatedPaintStrokesPerTick, 24);
        const auto batch_limit = static_cast<std::size_t>(std::max(1, std::min(raw_limit > 0 ? raw_limit : 24, 64)));
        for (std::size_t offset = 0; offset < strokes.size(); offset += batch_limit)
        {
            const auto count = std::min(batch_limit, strokes.size() - offset);
            std::string failure{};
            if (!sdk_call_server_paint_batch(ctx, strokes, offset, count, failure))
            {
                stats.server_failed += static_cast<int>(strokes.size() - offset);
                if (stats.first_failure.empty())
                {
                    stats.first_failure = failure.empty() ? "server_paint_batch_failed" : failure;
                }
                return false;
            }
            stats.server_sent += static_cast<int>(count);
            ++stats.batch_calls;
            stats.server_rpc = "ServerPaintBatch";
            Sleep(16);
        }
        return stats.server_sent == static_cast<int>(strokes.size());
    }

    auto sdk_stats_metadata(const char* prefix, const SdkReplicatedStats& stats) -> std::string
    {
        std::string name(prefix);
        return ",\"" + name + "_requested\":" + std::to_string(stats.requested) +
               ",\"" + name + "_server_sent\":" + std::to_string(stats.server_sent) +
               ",\"" + name + "_server_failed\":" + std::to_string(stats.server_failed) +
               ",\"" + name + "_batch_calls\":" + std::to_string(stats.batch_calls) +
               ",\"" + name + "_single_calls\":" + std::to_string(stats.single_calls) +
               ",\"" + name + "_client_mirror_sent\":" + std::to_string(stats.client_mirror_sent) +
               ",\"" + name + "_client_mirror_failed\":" + std::to_string(stats.client_mirror_failed) +
               ",\"" + name + "_server_rpc\":\"" + json_escape(stats.server_rpc) + "\"" +
               ",\"" + name + "_client_mirror_rpc\":\"" + json_escape(stats.client_mirror_rpc) + "\"" +
               ",\"" + name + "_first_failure\":\"" + json_escape(stats.first_failure) + "\"";
    }

    struct SdkFrontColorStats
    {
        int count{0};
        double min_rgb{0.0};
        double max_rgb{0.0};
        double avg_rgb{0.0};
        int whiteish_samples{0};
        bool all_whiteish{false};
    };

    auto sdk_front_color_stats(const std::vector<meccha_sdk::FPaintStroke>& strokes) -> SdkFrontColorStats
    {
        SdkFrontColorStats stats{};
        stats.count = static_cast<int>(strokes.size());
        bool initialized = false;
        double sum = 0.0;
        int channels = 0;
        for (const auto& stroke : strokes)
        {
            const double values[]{
                stroke.ChannelData.AlbedoColor.R,
                stroke.ChannelData.AlbedoColor.G,
                stroke.ChannelData.AlbedoColor.B,
            };
            bool sample_whiteish = true;
            for (const auto value : values)
            {
                if (!initialized)
                {
                    stats.min_rgb = value;
                    stats.max_rgb = value;
                    initialized = true;
                }
                stats.min_rgb = std::min(stats.min_rgb, value);
                stats.max_rgb = std::max(stats.max_rgb, value);
                sum += value;
                ++channels;
                if (value < 0.97)
                {
                    sample_whiteish = false;
                }
            }
            if (sample_whiteish)
            {
                ++stats.whiteish_samples;
            }
        }
        stats.avg_rgb = channels > 0 ? sum / static_cast<double>(channels) : 0.0;
        stats.all_whiteish = stats.count > 0 && stats.whiteish_samples == stats.count;
        return stats;
    }

    auto sdk_front_color_metadata(const SdkFrontColorStats& stats) -> std::string
    {
        return ",\"front_rgb_count\":" + std::to_string(stats.count) +
               ",\"front_rgb_min\":" + std::to_string(stats.min_rgb) +
               ",\"front_rgb_max\":" + std::to_string(stats.max_rgb) +
               ",\"front_rgb_avg\":" + std::to_string(stats.avg_rgb) +
               ",\"front_rgb_whiteish_samples\":" + std::to_string(stats.whiteish_samples) +
               ",\"front_rgb_all_whiteish\":" + json_bool(stats.all_whiteish);
    }

    auto sdk_function_caller(Reflection& ref, std::uintptr_t function) -> std::uintptr_t
    {
        const auto owner_class = safe_read<std::uintptr_t>(function + OffOuter);
        if (!owner_class)
        {
            return 0;
        }
        std::uintptr_t fallback = 0;
        std::uintptr_t cdo = 0;
        ref.for_each_object([&](std::uintptr_t object) {
            if (!object || address_in_main_module(object))
            {
                return false;
            }
            if (safe_read<std::uintptr_t>(object + OffClass) != owner_class)
            {
                return false;
            }
            if (!fallback)
            {
                fallback = object;
            }
            if ((safe_read<std::uint32_t>(object + OffObjectFlags, 0) & RFClassDefaultObject) != 0)
            {
                cdo = object;
                return true;
            }
            return false;
        });
        return cdo ? cdo : (fallback ? fallback : owner_class);
    }

    auto sdk_read_return_object_param(Reflection& ref, std::uintptr_t function, std::uint8_t* params) -> std::uintptr_t
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            if (lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName))) == "returnvalue")
            {
                return sdk_read_object(prop, params);
            }
        }
        return 0;
    }

    auto sdk_read_return_number_param(Reflection& ref, std::uintptr_t function, std::uint8_t* params, double& value) -> bool
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            if (lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName))) == "returnvalue")
            {
                value = sdk_read_number(ref, prop, params);
                return std::isfinite(value);
            }
        }
        return false;
    }

    auto sdk_read_rotator(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, meccha_sdk::FRotator& out) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        auto* base = container + offset;
        const auto st = struct_type(ref, prop, {"Pitch", "Yaw", "Roll"});
        if (st)
        {
            bool read = false;
            if (const auto p = find_property_any(ref, st, {"Pitch"})) { out.Pitch = sdk_read_number(ref, p, base); read = true; }
            if (const auto p = find_property_any(ref, st, {"Yaw"})) { out.Yaw = sdk_read_number(ref, p, base); read = true; }
            if (const auto p = find_property_any(ref, st, {"Roll"})) { out.Roll = sdk_read_number(ref, p, base); read = true; }
            return read && std::isfinite(out.Pitch) && std::isfinite(out.Yaw) && std::isfinite(out.Roll);
        }
        const auto size = prop_element_size(prop);
        if (size >= 24)
        {
            const auto* values = reinterpret_cast<double*>(base);
            out = {values[0], values[1], values[2]};
            return true;
        }
        if (size >= 12)
        {
            const auto* values = reinterpret_cast<float*>(base);
            out = {values[0], values[1], values[2]};
            return true;
        }
        return false;
    }

    auto sdk_read_return_vector3_param(Reflection& ref, std::uintptr_t function, std::uint8_t* params, meccha_sdk::FVector& value) -> bool
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            if (lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName))) == "returnvalue")
            {
                return sdk_read_vector3(ref, prop, params, value);
            }
        }
        return false;
    }

    auto sdk_read_return_rotator_param(Reflection& ref, std::uintptr_t function, std::uint8_t* params, meccha_sdk::FRotator& value) -> bool
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            if (lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName))) == "returnvalue")
            {
                return sdk_read_rotator(ref, prop, params, value);
            }
        }
        return false;
    }

    auto sdk_call_no_params_return_object(Reflection& ref, std::uintptr_t object, const char* function_name, std::string& failure) -> std::uintptr_t
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            failure = std::string(function_name) + "_unavailable";
            return 0;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 4096)
        {
            failure = std::string(function_name) + "_params_size_invalid";
            return 0;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(0, params_size)), 0);
        if (!process_event(object, function, params.data(), failure))
        {
            failure = std::string(function_name) + "_process_event_failed:" + failure;
            return 0;
        }
        return sdk_read_return_object_param(ref, function, params.data());
    }

    auto sdk_call_no_params_return_number(Reflection& ref, std::uintptr_t object, const char* function_name, double& value) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 4096)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(0, params_size)), 0);
        std::string failure{};
        return process_event(object, function, params.data(), failure) &&
               sdk_read_return_number_param(ref, function, params.data(), value);
    }

    auto sdk_call_no_params_return_vector3(Reflection& ref, std::uintptr_t object, const char* function_name, meccha_sdk::FVector& value) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 4096)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(0, params_size)), 0);
        std::string failure{};
        return process_event(object, function, params.data(), failure) &&
               sdk_read_return_vector3_param(ref, function, params.data(), value);
    }

    auto sdk_call_no_params_return_rotator(Reflection& ref, std::uintptr_t object, const char* function_name, meccha_sdk::FRotator& value) -> bool
    {
        const auto function = ref.find_function(object, function_name);
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size < 0 || params_size > 4096)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(0, params_size)), 0);
        std::string failure{};
        return process_event(object, function, params.data(), failure) &&
               sdk_read_return_rotator_param(ref, function, params.data(), value);
    }

    auto sdk_write_object_property_by_name(Reflection& ref, std::uintptr_t object, const char* name, std::uintptr_t value) -> bool
    {
        for (auto cls = ref.class_ptr(object); cls; cls = safe_read<std::uintptr_t>(cls + OffSuperStruct))
        {
            const auto prop = ref.find_property(cls, name);
            const auto offset = prop ? prop_offset(prop) : -1;
            if (offset < 0)
            {
                continue;
            }
            __try
            {
                *reinterpret_cast<std::uintptr_t*>(object + static_cast<std::uintptr_t>(offset)) = value;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
        return false;
    }

    auto sdk_write_number_property_by_name(Reflection& ref, std::uintptr_t object, const char* name, double value) -> bool
    {
        for (auto cls = ref.class_ptr(object); cls; cls = safe_read<std::uintptr_t>(cls + OffSuperStruct))
        {
            const auto prop = ref.find_property(cls, name);
            if (prop && prop_offset(prop) >= 0)
            {
                return write_number(ref, prop, reinterpret_cast<std::uint8_t*>(object), value);
            }
        }
        return false;
    }

    auto sdk_write_enum_byte(Reflection&, std::uintptr_t prop, std::uint8_t* container, std::uint8_t value) -> bool
    {
        const auto offset = prop_offset(prop);
        if (offset < 0)
        {
            return false;
        }
        __try
        {
            *(container + offset) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto sdk_write_enum_property_by_name(Reflection& ref, std::uintptr_t object, const char* name, std::uint8_t value) -> bool
    {
        for (auto cls = ref.class_ptr(object); cls; cls = safe_read<std::uintptr_t>(cls + OffSuperStruct))
        {
            const auto prop = ref.find_property(cls, name);
            if (prop && prop_offset(prop) >= 0)
            {
                return sdk_write_enum_byte(ref, prop, reinterpret_cast<std::uint8_t*>(object), value);
            }
        }
        return false;
    }

    auto sdk_write_bool_property_by_name(Reflection&, std::uintptr_t object, const char* name, bool value) -> bool
    {
        Reflection ref{};
        std::string ignored{};
        if (!ref.init(ignored))
        {
            return false;
        }
        for (auto cls = ref.class_ptr(object); cls; cls = safe_read<std::uintptr_t>(cls + OffSuperStruct))
        {
            const auto prop = ref.find_property(cls, name);
            if (prop && prop_offset(prop) >= 0)
            {
                return write_bool(prop, reinterpret_cast<std::uint8_t*>(object), value);
            }
        }
        return false;
    }

    auto sdk_write_quat_identity(Reflection& ref, std::uintptr_t prop, std::uint8_t* container) -> bool
    {
        const auto offset = prop_offset(prop);
        const auto structure = struct_type(ref, prop, {"X", "Y", "Z", "W"});
        if (offset < 0 || !structure)
        {
            return false;
        }
        auto* base = container + offset;
        bool wrote = false;
        if (const auto p = find_property_any(ref, structure, {"X"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, structure, {"Y"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, structure, {"Z"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        if (const auto p = find_property_any(ref, structure, {"W"})) wrote = write_number(ref, p, base, 1.0) || wrote;
        return wrote;
    }

    auto sdk_write_transform(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, const meccha_sdk::FVector& location) -> bool
    {
        const auto offset = prop_offset(prop);
        const auto structure = struct_type(ref, prop, {"Rotation", "Translation", "Scale3D"});
        if (offset < 0 || !structure)
        {
            return false;
        }
        auto* base = container + offset;
        bool wrote = false;
        if (const auto p = find_property_any(ref, structure, {"Rotation"}))
        {
            wrote = sdk_write_quat_identity(ref, p, base) || wrote;
        }
        if (const auto p = find_property_any(ref, structure, {"Translation", "Location"}))
        {
            wrote = sdk_write_vector3(ref, p, base, location) || wrote;
        }
        if (const auto p = find_property_any(ref, structure, {"Scale3D", "Scale"}))
        {
            meccha_sdk::FVector scale{};
            scale.X = 1.0;
            scale.Y = 1.0;
            scale.Z = 1.0;
            wrote = sdk_write_vector3(ref, p, base, scale) || wrote;
        }
        return wrote;
    }

    auto sdk_write_rotator(Reflection& ref, std::uintptr_t prop, std::uint8_t* container, const meccha_sdk::FVector& direction) -> bool
    {
        const auto offset = prop_offset(prop);
        const auto structure = struct_type(ref, prop, {"Pitch", "Yaw", "Roll"});
        if (offset < 0 || !structure)
        {
            return false;
        }
        auto* base = container + offset;
        const auto horizontal = std::sqrt(direction.X * direction.X + direction.Y * direction.Y);
        const auto pitch = std::atan2(direction.Z, std::max(0.000001, horizontal)) * 180.0 / 3.14159265358979323846;
        const auto yaw = std::atan2(direction.Y, direction.X) * 180.0 / 3.14159265358979323846;
        bool wrote = false;
        if (const auto p = find_property_any(ref, structure, {"Pitch"})) wrote = write_number(ref, p, base, pitch) || wrote;
        if (const auto p = find_property_any(ref, structure, {"Yaw"})) wrote = write_number(ref, p, base, yaw) || wrote;
        if (const auto p = find_property_any(ref, structure, {"Roll"})) wrote = write_number(ref, p, base, 0.0) || wrote;
        return wrote;
    }

    auto sdk_make_rotator(const meccha_sdk::FVector& direction) -> meccha_sdk::FRotator
    {
        const auto horizontal = std::sqrt(direction.X * direction.X + direction.Y * direction.Y);
        meccha_sdk::FRotator rot{};
        rot.Pitch = std::atan2(direction.Z, std::max(0.000001, horizontal)) * 180.0 / 3.14159265358979323846;
        rot.Yaw = std::atan2(direction.Y, direction.X) * 180.0 / 3.14159265358979323846;
        rot.Roll = 0.0;
        return rot;
    }

    auto sdk_rotator_forward(const meccha_sdk::FRotator& rotator) -> meccha_sdk::FVector
    {
        const auto pitch = rotator.Pitch * 3.14159265358979323846 / 180.0;
        const auto yaw = rotator.Yaw * 3.14159265358979323846 / 180.0;
        const auto cp = std::cos(pitch);
        return sdk_vec_normalize({cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch)});
    }

    auto sdk_make_transform(const meccha_sdk::FVector& location) -> meccha_sdk::FTransform
    {
        meccha_sdk::FTransform transform{};
        transform.Rotation.W = 1.0;
        transform.Translation = location;
        transform.Scale3D.X = 1.0;
        transform.Scale3D.Y = 1.0;
        transform.Scale3D.Z = 1.0;
        return transform;
    }

    auto sdk_create_render_target(Reflection& ref, const SdkContext& ctx, int width, int height, std::string& failure) -> std::uintptr_t
    {
        const auto function = sdk_find_object_named(ref, "CreateRenderTarget2D");
        const auto caller = sdk_function_caller(ref, function);
        if (!function || !caller)
        {
            failure = "create_render_target_function_unavailable";
            return 0;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 4096)
        {
            failure = "create_render_target_params_size_invalid";
            return 0;
        }
        if (params_size != static_cast<int>(sizeof(meccha_sdk::KismetRenderingLibrary_CreateRenderTarget2D)))
        {
            failure = "create_render_target_typed_params_size_mismatch";
            return 0;
        }
        meccha_sdk::KismetRenderingLibrary_CreateRenderTarget2D params{};
        params.WorldContextObject = reinterpret_cast<void*>(ctx.pawn);
        params.Width = width;
        params.Height = height;
        params.Format = meccha_sdk::ETextureRenderTargetFormat::RTF_RGBA8_SRGB;
        params.ClearColor.R = 0.0f;
        params.ClearColor.G = 0.0f;
        params.ClearColor.B = 0.0f;
        params.ClearColor.A = 1.0f;
        params.bAutoGenerateMipMaps = false;
        params.bSupportUAVs = false;
        if (!process_event(caller, function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            failure = "create_render_target_process_event_failed:" + failure;
            return 0;
        }
        const auto rt = reinterpret_cast<std::uintptr_t>(params.ReturnValue);
        if (!rt)
        {
            failure = "create_render_target_return_null";
        }
        return rt;
    }

    auto sdk_spawn_actor_from_class(Reflection& ref,
                                    const SdkContext& ctx,
                                    std::uintptr_t actor_class,
                                    const meccha_sdk::FVector& location,
                                    std::string& failure) -> std::uintptr_t
    {
        const auto function = sdk_find_object_named(ref, "BeginDeferredActorSpawnFromClass");
        const auto caller = sdk_function_caller(ref, function);
        if (!function || !caller || !actor_class)
        {
            failure = "begin_deferred_spawn_function_unavailable";
            return 0;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size != static_cast<int>(sizeof(meccha_sdk::GameplayStatics_BeginDeferredActorSpawnFromClass)))
        {
            failure = "begin_deferred_spawn_typed_params_size_mismatch";
            return 0;
        }
        const auto transform = sdk_make_transform(location);
        meccha_sdk::GameplayStatics_BeginDeferredActorSpawnFromClass params{};
        params.WorldContextObject = reinterpret_cast<void*>(ctx.pawn);
        params.ActorClass = reinterpret_cast<void*>(actor_class);
        params.SpawnTransform = transform;
        params.CollisionHandlingOverride = meccha_sdk::ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        params.Owner = reinterpret_cast<void*>(ctx.pawn);
        params.TransformScaleMethod = meccha_sdk::ESpawnActorScaleMethod::SelectDefaultAtRuntime;
        std::string begin_failure{};
        if (!process_event(caller, function, reinterpret_cast<std::uint8_t*>(&params), begin_failure) || !params.ReturnValue)
        {
            failure = "begin_deferred_spawn_process_event_failed:" + begin_failure;
            return 0;
        }

        auto actor = reinterpret_cast<std::uintptr_t>(params.ReturnValue);
        const auto finish = sdk_find_object_named(ref, "FinishSpawningActor");
        const auto finish_caller = sdk_function_caller(ref, finish);
        const auto finish_size = safe_read<int>(finish + OffPropertiesSize, 0);
        if (finish && finish_caller && finish_size == static_cast<int>(sizeof(meccha_sdk::GameplayStatics_FinishSpawningActor)))
        {
            meccha_sdk::GameplayStatics_FinishSpawningActor finish_params{};
            finish_params.Actor = reinterpret_cast<void*>(actor);
            finish_params.SpawnTransform = transform;
            finish_params.TransformScaleMethod = meccha_sdk::ESpawnActorScaleMethod::SelectDefaultAtRuntime;
            std::string finish_failure{};
            if (process_event(finish_caller, finish, reinterpret_cast<std::uint8_t*>(&finish_params), finish_failure) && finish_params.ReturnValue)
            {
                actor = reinterpret_cast<std::uintptr_t>(finish_params.ReturnValue);
            }
        }
        failure.clear();
        return actor;
    }

    auto sdk_set_actor_capture_transform(Reflection& ref,
                                         std::uintptr_t actor,
                                         const meccha_sdk::FVector& location,
                                         const meccha_sdk::FVector& direction) -> bool
    {
        bool ok = false;
        if (const auto function = ref.find_function(actor, "K2_SetActorLocation"))
        {
            const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
            if (params_size == static_cast<int>(sizeof(meccha_sdk::Actor_K2_SetActorLocation)))
            {
                meccha_sdk::Actor_K2_SetActorLocation params{};
                params.NewLocation = location;
                params.bSweep = false;
                params.bTeleport = true;
                std::string failure{};
                ok = process_event(actor, function, reinterpret_cast<std::uint8_t*>(&params), failure) || ok;
            }
        }
        if (const auto function = ref.find_function(actor, "K2_SetActorRotation"))
        {
            const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
            if (params_size == static_cast<int>(sizeof(meccha_sdk::Actor_K2_SetActorRotation)))
            {
                meccha_sdk::Actor_K2_SetActorRotation params{};
                params.NewRotation = sdk_make_rotator(direction);
                params.bTeleportPhysics = true;
                std::string failure{};
                ok = process_event(actor, function, reinterpret_cast<std::uint8_t*>(&params), failure) || ok;
            }
        }
        return ok;
    }

    auto sdk_project_world_to_screen(Reflection& ref,
                                     const SdkContext& ctx,
                                     const meccha_sdk::FVector& world,
                                     double& x,
                                     double& y) -> bool
    {
        const auto function = ref.find_function(ctx.controller, "ProjectWorldLocationToScreen");
        if (!function)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size <= 0 || params_size > 4096)
        {
            return false;
        }
        std::vector<std::uint8_t> params(static_cast<std::size_t>(params_size), 0);
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue")
            {
                continue;
            }
            if ((contains_text(name, "world") || contains_text(name, "location")) && !contains_text(name, "screen"))
            {
                sdk_write_vector3(ref, prop, params.data(), world);
            }
            else if (contains_text(name, "viewport"))
            {
                write_bool(prop, params.data(), false);
            }
        }
        std::string failure{};
        if (!process_event(ctx.controller, function, params.data(), failure) || !read_return_bool(ref, function, params.data()))
        {
            return false;
        }
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            const auto name = lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName)));
            if (name == "returnvalue")
            {
                continue;
            }
            if (contains_text(name, "screen"))
            {
                return sdk_read_vector2(ref, prop, params.data(), x, y);
            }
        }
        return false;
    }

    auto sdk_read_return_linear_color(Reflection& ref, std::uintptr_t function, std::uint8_t* params, Color& color) -> bool
    {
        for (auto prop = safe_read<std::uintptr_t>(function + OffChildProperties); prop; prop = safe_read<std::uintptr_t>(prop + OffFFieldNext))
        {
            if (lower_copy(ref.names.resolve(safe_read<std::uint32_t>(prop + OffFFieldName))) != "returnvalue")
            {
                continue;
            }
            const auto offset = prop_offset(prop);
            const auto structure = struct_type(ref, prop, {"R", "G", "B", "A"});
            if (offset < 0 || !structure)
            {
                return false;
            }
            auto* base = params + offset;
            bool read = false;
            if (const auto p = find_property_any(ref, structure, {"R"})) { color.r = sdk_read_number(ref, p, base); read = true; }
            if (const auto p = find_property_any(ref, structure, {"G"})) { color.g = sdk_read_number(ref, p, base); read = true; }
            if (const auto p = find_property_any(ref, structure, {"B"})) { color.b = sdk_read_number(ref, p, base); read = true; }
            color.r = clamp01(color.r);
            color.g = clamp01(color.g);
            color.b = clamp01(color.b);
            color.roughness = 0.65;
            color.metallic = 0.0;
            return read;
        }
        return false;
    }

    auto sdk_read_render_target_raw_pixel(Reflection& ref,
                                          const SdkContext& ctx,
                                          std::uintptr_t render_target,
                                          int x,
                                          int y,
                                          Color& color,
                                          std::uintptr_t& function_used) -> bool
    {
        const auto function = sdk_find_object_named(ref, "ReadRenderTargetPixel");
        const auto caller = sdk_function_caller(ref, function);
        function_used = function;
        if (!function || !caller || !render_target)
        {
            return false;
        }
        const auto params_size = safe_read<int>(function + OffPropertiesSize, 0);
        if (params_size != static_cast<int>(sizeof(meccha_sdk::KismetRenderingLibrary_ReadRenderTargetPixel)))
        {
            return false;
        }
        meccha_sdk::KismetRenderingLibrary_ReadRenderTargetPixel params{};
        params.WorldContextObject = reinterpret_cast<void*>(ctx.pawn);
        params.TextureRenderTarget = reinterpret_cast<void*>(render_target);
        params.X = x;
        params.Y = y;
        std::string failure{};
        if (!process_event(caller, function, reinterpret_cast<std::uint8_t*>(&params), failure))
        {
            return false;
        }
        color.r = static_cast<double>(params.ReturnValue.R) / 255.0;
        color.g = static_cast<double>(params.ReturnValue.G) / 255.0;
        color.b = static_cast<double>(params.ReturnValue.B) / 255.0;
        color.roughness = 0.65;
        color.metallic = 0.0;
        return true;
    }

    auto sdk_configure_scene_capture_component_typed(std::uintptr_t capture_component,
                                                     std::uintptr_t render_target,
                                                     double fov_degrees = 90.0) -> bool
    {
        if (!capture_component || !render_target)
        {
            return false;
        }
        __try
        {
            *reinterpret_cast<std::uintptr_t*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent2D_TextureTarget) = render_target;
            *reinterpret_cast<std::uint8_t*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent_CaptureSource) =
                static_cast<std::uint8_t>(meccha_sdk::ESceneCaptureSource::FinalColorLDR);
            auto* capture_flags = reinterpret_cast<std::uint8_t*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent_CaptureFlags);
            *capture_flags = static_cast<std::uint8_t>(*capture_flags & ~0x03);
            *reinterpret_cast<bool*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent_bAlwaysPersistRenderingState) = true;
            *reinterpret_cast<std::uint8_t*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent2D_ProjectionType) =
                static_cast<std::uint8_t>(meccha_sdk::ECameraProjectionMode::Perspective);
            const auto fov = std::isfinite(fov_degrees) ? std::max(10.0, std::min(150.0, fov_degrees)) : 90.0;
            *reinterpret_cast<float*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent2D_FOVAngle) = static_cast<float>(fov);
            return *reinterpret_cast<std::uintptr_t*>(capture_component + meccha_sdk::FieldOffsets::SceneCaptureComponent2D_TextureTarget) == render_target;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    constexpr bool kEnableNativeSceneCaptureForF10 = true;

    auto sdk_capture_front_colors(Reflection& ref,
                                  const SdkContext& ctx,
                                  const SdkNativeFrontSampleResult& native_front,
                                  int target_width,
                                  int target_height) -> SdkFrontCaptureResult
    {
        SdkFrontCaptureResult out{};
        if (native_front.samples.empty())
        {
            out.failure = "front_capture_no_surface_samples";
            return out;
        }
        const auto viewport = sdk_get_viewport_info(ref, ctx);
        out.width = viewport.width;
        out.height = viewport.height;
        if (out.width <= 0 || out.height <= 0)
        {
            out.failure = "front_capture_viewport_unavailable";
            return out;
        }
        if (!kEnableNativeSceneCaptureForF10)
        {
            out.failure = "front_capture_backend_disabled_after_d3d12_crash";
            return out;
        }
        const int viewport_width = out.width;
        const int viewport_height = out.height;
        out.viewport_width = viewport_width;
        out.viewport_height = viewport_height;
        out.viewport_aspect = static_cast<double>(std::max(1, viewport_width)) / static_cast<double>(std::max(1, viewport_height));
        if (target_width > 0 && target_height > 0)
        {
            out.requested_texture_width = target_width;
            out.requested_texture_height = target_height;
            int capture_width = std::max(1, target_width);
            int capture_height = std::max(1, target_height);
            if (out.viewport_aspect >= 1.0)
            {
                capture_height = std::max(1, target_height);
                capture_width = std::max(1, static_cast<int>(std::round(static_cast<double>(capture_height) * out.viewport_aspect)));
            }
            else
            {
                capture_width = std::max(1, target_width);
                capture_height = std::max(1, static_cast<int>(std::round(static_cast<double>(capture_width) / std::max(0.000001, out.viewport_aspect))));
            }
            constexpr int max_capture_dimension = 4096;
            const auto max_dimension = std::max(capture_width, capture_height);
            if (max_dimension > max_capture_dimension)
            {
                const auto scale = static_cast<double>(max_capture_dimension) / static_cast<double>(max_dimension);
                capture_width = std::max(1, static_cast<int>(std::round(static_cast<double>(capture_width) * scale)));
                capture_height = std::max(1, static_cast<int>(std::round(static_cast<double>(capture_height) * scale)));
            }
            out.width = capture_width;
            out.height = capture_height;
            out.capture_resolution_source = "texture_resolution_viewport_aspect";
        }
        out.capture_aspect = static_cast<double>(std::max(1, out.width)) / static_cast<double>(std::max(1, out.height));
        const double capture_scale_x = static_cast<double>(out.width) / static_cast<double>(viewport_width);
        const double capture_scale_y = static_cast<double>(out.height) / static_cast<double>(viewport_height);
        out.capture_scale_x = capture_scale_x;
        out.capture_scale_y = capture_scale_y;
        const auto center_ray = sdk_deproject_screen_position(ref, ctx, static_cast<double>(viewport_width) * 0.5, static_cast<double>(viewport_height) * 0.5);
        if (!center_ray.ok)
        {
            out.failure = "front_capture_camera_deproject_failed:" + center_ray.failure;
            return out;
        }
        auto capture_location = center_ray.location;
        auto capture_direction = center_ray.direction;
        const auto left_ray = sdk_deproject_screen_position(ref, ctx, 0.0, static_cast<double>(viewport_height) * 0.5);
        const auto right_ray = sdk_deproject_screen_position(ref, ctx, static_cast<double>(std::max(1, viewport_width - 1)), static_cast<double>(viewport_height) * 0.5);
        if (left_ray.ok && right_ray.ok)
        {
            const auto left_dir = sdk_vec_normalize(left_ray.direction);
            const auto right_dir = sdk_vec_normalize(right_ray.direction);
            const auto dot = std::max(-1.0, std::min(1.0, sdk_vec_dot(left_dir, right_dir)));
            const auto fov = std::acos(dot) * 180.0 / 3.14159265358979323846;
            if (std::isfinite(fov) && fov >= 10.0 && fov <= 150.0)
            {
                out.capture_fov = fov;
            }
        }
        std::string camera_failure{};
        out.camera_manager = sdk_call_no_params_return_object(ref, ctx.controller, "GetPlayerCameraManager", camera_failure);
        if (live_uobject(out.camera_manager))
        {
            meccha_sdk::FVector camera_location{};
            if (sdk_call_no_params_return_vector3(ref, out.camera_manager, "GetCameraLocation", camera_location))
            {
                capture_location = camera_location;
                out.camera_location_used = true;
                out.camera_location_source = "player_camera_manager";
            }
            meccha_sdk::FRotator camera_rotation{};
            if (sdk_call_no_params_return_rotator(ref, out.camera_manager, "GetCameraRotation", camera_rotation))
            {
                capture_direction = sdk_rotator_forward(camera_rotation);
                out.camera_rotation_used = true;
                out.camera_rotation_source = "player_camera_manager";
            }
            double camera_fov = 0.0;
            if (sdk_call_no_params_return_number(ref, out.camera_manager, "GetFOVAngle", camera_fov) &&
                std::isfinite(camera_fov) && camera_fov >= 10.0 && camera_fov <= 150.0)
            {
                out.capture_fov = camera_fov;
                out.camera_fov_used = true;
                out.camera_fov_source = "player_camera_manager";
            }
        }
        out.capture_location = capture_location;
        out.capture_direction = sdk_vec_normalize(capture_direction);
        std::string failure{};
        out.render_target = sdk_create_render_target(ref, ctx, out.width, out.height, failure);
        out.render_target_created = out.render_target != 0;
        if (!out.render_target)
        {
            out.failure = failure.empty() ? "front_capture_render_target_unavailable" : failure;
            return out;
        }
        const auto scene_capture_class = ref.find_class("SceneCapture2D");
        out.capture_actor = sdk_spawn_actor_from_class(ref, ctx, scene_capture_class, out.capture_location, failure);
        out.capture_actor_spawned = out.capture_actor != 0;
        if (!out.capture_actor)
        {
            out.failure = failure.empty() ? "front_capture_actor_spawn_failed" : failure;
            return out;
        }
        sdk_set_actor_capture_transform(ref, out.capture_actor, out.capture_location, out.capture_direction);
        out.capture_component = safe_read<std::uintptr_t>(out.capture_actor + meccha_sdk::FieldOffsets::SceneCapture2D_CaptureComponent2D, 0);
        if (!out.capture_component)
        {
            out.capture_component = sdk_call_no_params_return_object(ref, out.capture_actor, "GetCaptureComponent2D", failure);
        }
        out.capture_component_found = out.capture_component != 0;
        if (!out.capture_component)
        {
            out.failure = failure.empty() ? "front_capture_component_unavailable" : failure;
            sdk_call_no_params(ref, out.capture_actor, "K2_DestroyActor");
            return out;
        }
        out.texture_target_written = sdk_configure_scene_capture_component_typed(out.capture_component, out.render_target, out.capture_fov);
        out.hide_component_called = native_front.mesh && sdk_call_object_param(ref, out.capture_component, "HideComponent", native_front.mesh);
        if (!out.texture_target_written)
        {
            out.failure = "front_capture_texture_target_write_failed";
            sdk_call_no_params(ref, out.capture_actor, "K2_DestroyActor");
            return out;
        }
        out.capture_scene_called = sdk_call_no_params(ref, out.capture_component, "CaptureScene");
        if (!out.capture_scene_called)
        {
            out.failure = "front_capture_scene_failed";
            sdk_call_no_params(ref, out.capture_actor, "K2_DestroyActor");
            return out;
        }
        Sleep(50);

        out.samples.reserve(native_front.samples.size());
        double sum = 0.0;
        int channels = 0;
        bool initialized = false;
        for (const auto& surface : native_front.samples)
        {
            ++out.project_attempts;
            double sx = 0.0;
            double sy = 0.0;
            if (!sdk_project_world_to_screen(ref, ctx, surface.world_position, sx, sy))
            {
                ++out.missing_color;
                continue;
            }
            ++out.project_success;
            const auto px = std::max(0, std::min(out.width - 1, static_cast<int>(std::round(sx * capture_scale_x))));
            const auto py = std::max(0, std::min(out.height - 1, static_cast<int>(std::round(sy * capture_scale_y))));
            Color color{};
            ++out.read_attempts;
            if (!sdk_read_render_target_raw_pixel(ref, ctx, out.render_target, px, py, color, out.read_function))
            {
                ++out.missing_color;
                continue;
            }
            ++out.read_success;
            FrontSample sample = surface;
            sample.r = std::max(0.01, std::min(0.99, color.r));
            sample.g = std::max(0.01, std::min(0.99, color.g));
            sample.b = std::max(0.01, std::min(0.99, color.b));
            sample.metallic = 0.0;
            sample.roughness = 0.65;
            sample.radius = std::max(0.0015, std::min(0.0035, 2.5 / static_cast<double>(std::max(out.width, out.height))));
            sample.floor_like = false;
            sample.atlas_priority = 11;
            sample.atlas_radius = 1;
            sample.atlas_weight = 72.0;
            out.samples.push_back(sample);
            const double values[]{sample.r, sample.g, sample.b};
            bool whiteish = true;
            for (const auto value : values)
            {
                if (!initialized)
                {
                    out.rgb_min = value;
                    out.rgb_max = value;
                    initialized = true;
                }
                out.rgb_min = std::min(out.rgb_min, value);
                out.rgb_max = std::max(out.rgb_max, value);
                sum += value;
                ++channels;
                if (value < 0.97)
                {
                    whiteish = false;
                }
            }
            if (whiteish)
            {
                ++out.whiteish_samples;
            }
        }
        out.rgb_avg = channels > 0 ? sum / static_cast<double>(channels) : 0.0;
        out.luma_range = out.rgb_max - out.rgb_min;
        out.uniform = out.samples.size() > 0 && out.luma_range < 0.006;
        out.all_whiteish = out.samples.size() > 0 && out.whiteish_samples == static_cast<int>(out.samples.size());
        out.ok = static_cast<int>(out.samples.size()) >= native_front.min_front_hits && !out.uniform && !out.all_whiteish;
        out.failure = out.ok ? "ok" : (out.samples.empty() ? "front_capture_color_empty" : "front_capture_quality_failed");
        sdk_call_no_params(ref, out.capture_actor, "K2_DestroyActor");
        return out;
    }

    auto sdk_capture_metadata(const SdkFrontCaptureResult& capture) -> std::string
    {
        return ",\"front_capture_ok\":" + std::string(json_bool(capture.ok)) +
               ",\"front_capture_failure\":\"" + json_escape(capture.failure) + "\"" +
               ",\"capture_resolution\":\"" + std::to_string(capture.width) + "x" + std::to_string(capture.height) + "\"" +
               ",\"capture_fov\":" + std::to_string(capture.capture_fov) +
               ",\"capture_resolution_source\":\"" + json_escape(capture.capture_resolution_source) + "\"" +
               ",\"capture_requested_texture_width\":" + std::to_string(capture.requested_texture_width) +
               ",\"capture_requested_texture_height\":" + std::to_string(capture.requested_texture_height) +
               ",\"capture_viewport_width\":" + std::to_string(capture.viewport_width) +
               ",\"capture_viewport_height\":" + std::to_string(capture.viewport_height) +
               ",\"capture_viewport_aspect\":" + std::to_string(capture.viewport_aspect) +
               ",\"capture_aspect\":" + std::to_string(capture.capture_aspect) +
               ",\"capture_scale_x\":" + std::to_string(capture.capture_scale_x) +
               ",\"capture_scale_y\":" + std::to_string(capture.capture_scale_y) +
               ",\"front_capture_render_target\":\"" + hex_address(capture.render_target) + "\"" +
               ",\"front_capture_actor\":\"" + hex_address(capture.capture_actor) + "\"" +
               ",\"front_capture_component\":\"" + hex_address(capture.capture_component) + "\"" +
               ",\"front_capture_read_function\":\"" + hex_address(capture.read_function) + "\"" +
               ",\"front_capture_render_target_created\":" + std::string(json_bool(capture.render_target_created)) +
               ",\"front_capture_actor_spawned\":" + std::string(json_bool(capture.capture_actor_spawned)) +
               ",\"front_capture_component_found\":" + std::string(json_bool(capture.capture_component_found)) +
               ",\"front_capture_texture_target_written\":" + std::string(json_bool(capture.texture_target_written)) +
               ",\"front_capture_hide_component_called\":" + std::string(json_bool(capture.hide_component_called)) +
               ",\"front_capture_scene_called\":" + std::string(json_bool(capture.capture_scene_called)) +
               ",\"capture_camera_manager\":\"" + hex_address(capture.camera_manager) + "\"" +
               ",\"capture_camera_location_used\":" + std::string(json_bool(capture.camera_location_used)) +
               ",\"capture_camera_rotation_used\":" + std::string(json_bool(capture.camera_rotation_used)) +
               ",\"capture_camera_fov_used\":" + std::string(json_bool(capture.camera_fov_used)) +
               ",\"capture_camera_location_source\":\"" + json_escape(capture.camera_location_source) + "\"" +
               ",\"capture_camera_rotation_source\":\"" + json_escape(capture.camera_rotation_source) + "\"" +
               ",\"capture_camera_fov_source\":\"" + json_escape(capture.camera_fov_source) + "\"" +
               ",\"capture_location_x\":" + std::to_string(capture.capture_location.X) +
               ",\"capture_location_y\":" + std::to_string(capture.capture_location.Y) +
               ",\"capture_location_z\":" + std::to_string(capture.capture_location.Z) +
               ",\"capture_direction_x\":" + std::to_string(capture.capture_direction.X) +
               ",\"capture_direction_y\":" + std::to_string(capture.capture_direction.Y) +
               ",\"capture_direction_z\":" + std::to_string(capture.capture_direction.Z) +
               ",\"front_capture_project_attempts\":" + std::to_string(capture.project_attempts) +
               ",\"front_capture_project_success\":" + std::to_string(capture.project_success) +
               ",\"front_capture_read_attempts\":" + std::to_string(capture.read_attempts) +
               ",\"front_capture_read_success\":" + std::to_string(capture.read_success) +
               ",\"front_capture_missing_color\":" + std::to_string(capture.missing_color) +
               ",\"front_rgb_min\":" + std::to_string(capture.rgb_min) +
               ",\"front_rgb_max\":" + std::to_string(capture.rgb_max) +
               ",\"front_rgb_avg\":" + std::to_string(capture.rgb_avg) +
               ",\"front_luma_range\":" + std::to_string(capture.luma_range) +
               ",\"front_rgb_whiteish_samples\":" + std::to_string(capture.whiteish_samples) +
               ",\"front_rgb_uniform\":" + std::string(json_bool(capture.uniform)) +
               ",\"front_rgb_all_whiteish\":" + std::string(json_bool(capture.all_whiteish));
    }

    struct SdkAtlasSideBackResult
    {
        std::vector<FrontSample> samples{};
        int attempts{0};
        int success{0};
        int owner_hits{0};
        int uv_hits{0};
        int side_hits{0};
        int back_hits{0};
        int duplicate_texels{0};
        int nearest_sources{0};
        std::string failure{"not_run"};
    };

    struct SdkTextureAtlasStats
    {
        int width{0};
        int height{0};
        int direct_texels{0};
        int filled_by_extension{0};
        int preserved_original{0};
        int source_samples{0};
        int worker_threads{1};
    };

    struct SdkTextureAtlas
    {
        bool ok{false};
        std::string failure{"atlas_not_built"};
        std::vector<std::uint8_t> albedo{};
        std::vector<std::uint8_t> painted_mask{};
        std::uint64_t hash{1469598103934665603ULL};
        SdkTextureAtlasStats stats{};
    };

    struct SdkAtlasStrokePlan
    {
        std::vector<meccha_sdk::FPaintStroke> strokes{};
        int stroke_cap{160000};
        int target_strokes{30000};
        int source_texels{0};
        int merged_8{0};
        int merged_4{0};
        int merged_2{0};
        int singles{0};
        double merge_error_max{2.0 / 255.0};
        bool cap_exceeded{false};
        std::string failure{"not_run"};
    };

    auto sdk_byte_from_unit(double value) -> std::uint8_t
    {
        return static_cast<std::uint8_t>(std::round(clamp01(value) * 255.0));
    }

    auto sdk_texel_key(double u, double v, int width, int height) -> std::uint64_t
    {
        const auto x = static_cast<std::uint64_t>(std::max(0, std::min(std::max(0, width - 1), static_cast<int>(clamp01(u) * static_cast<double>(std::max(1, width))))));
        const auto y = static_cast<std::uint64_t>(std::max(0, std::min(std::max(0, height - 1), static_cast<int>(clamp01(v) * static_cast<double>(std::max(1, height))))));
        return (y << 32) | x;
    }

    auto sdk_collect_atlas_side_back_samples(Reflection& ref,
                                             const SdkContext& ctx,
                                             const SdkNativeFrontSampleResult& native_front,
                                             const std::vector<FrontSample>& direct_samples,
                                             int width,
                                             int height) -> SdkAtlasSideBackResult
    {
        SdkAtlasSideBackResult out{};
        out.failure = "side_back_unavailable";
        if (direct_samples.empty())
        {
            out.failure = "side_back_no_direct_samples";
            return out;
        }
        auto query = native_front.query ? native_front.query : sdk_find_screen_space_brush_query(ref, ctx);
        if (!query || !sdk_configure_screen_space_brush_query(ref, query, ctx.pawn, native_front.mesh))
        {
            out.failure = "side_back_query_unavailable";
            return out;
        }

        std::unordered_set<std::uint64_t> unique_texels{};
        unique_texels.reserve(direct_samples.size() + 32768);
        meccha_sdk::FVector min_world = direct_samples.front().world_position;
        meccha_sdk::FVector max_world = direct_samples.front().world_position;
        meccha_sdk::FVector sum_world{};
        for (const auto& sample : direct_samples)
        {
            unique_texels.insert(sdk_texel_key(sample.u, sample.v, width, height));
            sum_world = sdk_vec_add(sum_world, sample.world_position);
            min_world.X = std::min(min_world.X, sample.world_position.X);
            min_world.Y = std::min(min_world.Y, sample.world_position.Y);
            min_world.Z = std::min(min_world.Z, sample.world_position.Z);
            max_world.X = std::max(max_world.X, sample.world_position.X);
            max_world.Y = std::max(max_world.Y, sample.world_position.Y);
            max_world.Z = std::max(max_world.Z, sample.world_position.Z);
        }
        const auto center = sdk_vec_mul(sum_world, 1.0 / static_cast<double>(std::max<std::size_t>(1, direct_samples.size())));
        const auto extent = sdk_vec_sub(max_world, min_world);
        const auto half_width = std::max(80.0, std::min(260.0, std::max(std::abs(extent.X), std::abs(extent.Y)) * 0.78));
        const auto half_height = std::max(100.0, std::min(320.0, std::abs(extent.Z) * 0.78));
        const auto ray_distance = std::max(260.0, std::min(820.0, std::max({std::abs(extent.X), std::abs(extent.Y), std::abs(extent.Z), 160.0}) * 2.8));
        auto base_outward = sdk_vec_sub(native_front.first_ray_location, center);
        base_outward.Z = 0.0;
        if (sdk_vec_len(base_outward) < 0.01)
        {
            base_outward = {1.0, 0.0, 0.0};
        }
        base_outward = sdk_vec_normalize(base_outward);
        auto frame_forward = sdk_vec_normalize(sdk_vec_sub(center, native_front.first_ray_location));
        if (sdk_vec_len(frame_forward) < 0.01)
        {
            frame_forward = sdk_vec_normalize(native_front.first_ray_direction);
        }
        auto frame_right = sdk_vec_normalize(sdk_vec_cross({0.0, 0.0, 1.0}, frame_forward));
        if (sdk_vec_len(frame_right) < 0.01)
        {
            frame_right = {1.0, 0.0, 0.0};
        }
        auto frame_up = sdk_vec_normalize(sdk_vec_cross(frame_forward, frame_right));
        if (sdk_vec_len(frame_up) < 0.01)
        {
            frame_up = {0.0, 0.0, 1.0};
        }
        auto rotate_z = [](const meccha_sdk::FVector& v, double degrees) {
            const auto radians = degrees * 3.14159265358979323846 / 180.0;
            const auto c = std::cos(radians);
            const auto s = std::sin(radians);
            return meccha_sdk::FVector{v.X * c - v.Y * s, v.X * s + v.Y * c, v.Z};
        };
        auto rotate_yaw_pitch = [&](const meccha_sdk::FVector& v, double yaw_degrees, double pitch_degrees) {
            auto yawed = sdk_vec_normalize(rotate_z(v, yaw_degrees));
            yawed.Z = 0.0;
            if (sdk_vec_len(yawed) < 0.01)
            {
                yawed = base_outward;
            }
            yawed = sdk_vec_normalize(yawed);
            const auto pitch = pitch_degrees * 3.14159265358979323846 / 180.0;
            const auto c = std::cos(pitch);
            const auto s = std::sin(pitch);
            return sdk_vec_normalize({yawed.X * c, yawed.Y * c, s});
        };
        auto nearest_direct = [&](const meccha_sdk::FVector& world) -> const FrontSample* {
            const FrontSample* best = nullptr;
            double best_score = 1000000000000.0;
            for (const auto& sample : direct_samples)
            {
                const auto delta = sdk_vec_sub(sample.world_position, world);
                const auto right_delta = sdk_vec_dot(delta, frame_right);
                const auto up_delta = sdk_vec_dot(delta, frame_up);
                const auto forward_delta = sdk_vec_dot(delta, frame_forward);
                const auto score = right_delta * right_delta +
                                   up_delta * up_delta * 1.35 +
                                   forward_delta * forward_delta * 0.16;
                if (score < best_score)
                {
                    best_score = score;
                    best = &sample;
                }
            }
            return best;
        };
        constexpr std::size_t centerline_bin_count = 96;
        std::vector<const FrontSample*> centerline_bins(centerline_bin_count, nullptr);
        std::vector<double> centerline_scores(centerline_bin_count, 1000000000000.0);
        double min_up = 1000000000000.0;
        double max_up = -1000000000000.0;
        for (const auto& sample : direct_samples)
        {
            const auto delta = sdk_vec_sub(sample.world_position, center);
            const auto up_delta = sdk_vec_dot(delta, frame_up);
            min_up = std::min(min_up, up_delta);
            max_up = std::max(max_up, up_delta);
        }
        const auto up_span = std::max(1.0, max_up - min_up);
        for (const auto& sample : direct_samples)
        {
            const auto delta = sdk_vec_sub(sample.world_position, center);
            const auto right_delta = sdk_vec_dot(delta, frame_right);
            const auto up_delta = sdk_vec_dot(delta, frame_up);
            const auto forward_delta = sdk_vec_dot(delta, frame_forward);
            const auto normalized_up = std::max(0.0, std::min(0.999999, (up_delta - min_up) / up_span));
            const auto bin = std::min(centerline_bin_count - 1, static_cast<std::size_t>(normalized_up * static_cast<double>(centerline_bin_count)));
            const auto score = std::abs(right_delta) + std::abs(forward_delta) * 0.04;
            if (score < centerline_scores[bin])
            {
                centerline_scores[bin] = score;
                centerline_bins[bin] = &sample;
            }
        }
        std::vector<const FrontSample*> centerline_targets{};
        centerline_targets.reserve(centerline_bin_count);
        for (const auto* sample : centerline_bins)
        {
            if (sample)
            {
                centerline_targets.push_back(sample);
            }
        }
        auto try_ray = [&](const meccha_sdk::FVector& origin, const meccha_sdk::FVector& ray_dir, bool back_view) {
            ++out.attempts;
            const auto hit = sdk_query_brush_from_world_ray(ref, query, origin, ray_dir);
            if (!hit.params_ok || !hit.success)
            {
                if (out.failure == "side_back_unavailable")
                {
                    out.failure = hit.failure.empty() ? "side_back_query_no_hit" : hit.failure;
                }
                return;
            }
            ++out.success;
            const bool owner_hit = (hit.component && hit.component == native_front.mesh) ||
                                   sdk_object_is_or_belongs_to(ref, hit.actor, ctx.pawn) ||
                                   sdk_object_is_or_belongs_to(ref, hit.component, ctx.pawn);
            if (!owner_hit || !hit.has_uv || !std::isfinite(hit.u) || !std::isfinite(hit.v))
            {
                return;
            }
            ++out.owner_hits;
            ++out.uv_hits;
            const auto key = sdk_texel_key(hit.u, hit.v, width, height);
            if (!unique_texels.insert(key).second)
            {
                ++out.duplicate_texels;
                return;
            }
            const auto* nearest = nearest_direct(hit.world_position);
            if (!nearest)
            {
                return;
            }
            ++out.nearest_sources;
            FrontSample sample = *nearest;
            sample.u = clamp01(hit.u);
            sample.v = clamp01(hit.v);
            sample.world_position = hit.world_position;
            sample.normal = sdk_vec_normalize(hit.normal);
            sample.has_world_position = true;
            sample.radius = std::max(0.0015, nearest->radius);
            sample.floor_like = nearest->floor_like;
            sample.atlas_priority = sample.floor_like ? 8 : 7;
            sample.atlas_radius = 3;
            sample.atlas_weight = sample.floor_like ? 48.0 : 42.0;
            out.samples.push_back(sample);
            if (back_view)
            {
                ++out.back_hits;
            }
            else
            {
                ++out.side_hits;
            }
        };

        const double yaw_offsets[]{-150.0, -120.0, -90.0, -60.0, 60.0, 90.0, 120.0, 150.0, 180.0};
        const double pitch_offsets[]{-22.0, 0.0, 22.0};
        constexpr int grid_x = 24;
        constexpr int grid_y = 36;
        std::size_t virtual_view_index = 0;
        for (const auto yaw : yaw_offsets)
        {
            for (const auto pitch : pitch_offsets)
            {
                const auto outward = rotate_yaw_pitch(base_outward, yaw, pitch);
                const auto origin = sdk_vec_add(center, sdk_vec_mul(outward, ray_distance));
                const auto view_forward = sdk_vec_normalize(sdk_vec_sub(center, origin));
                auto view_right = sdk_vec_normalize(sdk_vec_cross({0.0, 0.0, 1.0}, view_forward));
                if (sdk_vec_len(view_right) < 0.01)
                {
                    view_right = {1.0, 0.0, 0.0};
                }
                auto view_up = sdk_vec_normalize(sdk_vec_cross(view_forward, view_right));
                if (sdk_vec_len(view_up) < 0.01)
                {
                    view_up = {0.0, 0.0, 1.0};
                }
                for (int y = 0; y < grid_y; ++y)
                {
                    for (int x = 0; x < grid_x; ++x)
                    {
                        const auto lx = ((static_cast<double>(x) + 0.5) / static_cast<double>(grid_x) - 0.5) * 2.0;
                        const auto ly = ((static_cast<double>(y) + 0.5) / static_cast<double>(grid_y) - 0.5) * 2.0;
                        const auto target = sdk_vec_add(sdk_vec_add(center, sdk_vec_mul(view_right, lx * half_width)), sdk_vec_mul(view_up, ly * half_height));
                        try_ray(origin, sdk_vec_normalize(sdk_vec_sub(target, origin)), std::abs(yaw) > 165.0);
                    }
                }
                constexpr int direct_target_rays_per_view = 1024;
                const auto seed_count = direct_samples.size();
                const auto stride = std::max<std::size_t>(1, seed_count / static_cast<std::size_t>(direct_target_rays_per_view));
                const auto view_offset = (virtual_view_index * 131u) % std::max<std::size_t>(1, seed_count);
                const auto target_rays = std::min<std::size_t>(static_cast<std::size_t>(direct_target_rays_per_view), seed_count);
                for (std::size_t i = 0; i < target_rays; ++i)
                {
                    const auto seed_index = (view_offset + i * stride) % seed_count;
                    const auto jitter_x = (static_cast<double>((i * 17u + virtual_view_index * 7u) % 11u) - 5.0) * 1.4;
                    const auto jitter_y = (static_cast<double>((i * 23u + virtual_view_index * 5u) % 13u) - 6.0) * 1.2;
                    const auto target = sdk_vec_add(sdk_vec_add(direct_samples[seed_index].world_position, sdk_vec_mul(view_right, jitter_x)),
                                                    sdk_vec_mul(view_up, jitter_y));
                    try_ray(origin, sdk_vec_normalize(sdk_vec_sub(target, origin)), std::abs(yaw) > 165.0);
                }
                ++virtual_view_index;
            }
        }
        const double centerline_yaw_offsets[]{-45.0, -30.0, 0.0, 30.0, 45.0};
        const double centerline_pitch_offsets[]{-38.0, 0.0, 38.0};
        const double centerline_jitter[][2]{{0.0, 0.0}, {4.0, 0.0}, {-4.0, 0.0}, {0.0, 5.0}, {0.0, -5.0}};
        for (const auto yaw : centerline_yaw_offsets)
        {
            for (const auto pitch : centerline_pitch_offsets)
            {
                const auto outward = rotate_yaw_pitch(base_outward, yaw, pitch);
                const auto origin = sdk_vec_add(center, sdk_vec_mul(outward, ray_distance));
                const auto view_forward = sdk_vec_normalize(sdk_vec_sub(center, origin));
                auto view_right = sdk_vec_normalize(sdk_vec_cross({0.0, 0.0, 1.0}, view_forward));
                if (sdk_vec_len(view_right) < 0.01)
                {
                    view_right = frame_right;
                }
                auto view_up = sdk_vec_normalize(sdk_vec_cross(view_forward, view_right));
                if (sdk_vec_len(view_up) < 0.01)
                {
                    view_up = frame_up;
                }
                for (const auto* target_seed : centerline_targets)
                {
                    for (const auto& jitter : centerline_jitter)
                    {
                        const auto target = sdk_vec_add(sdk_vec_add(target_seed->world_position, sdk_vec_mul(view_right, jitter[0])),
                                                        sdk_vec_mul(view_up, jitter[1]));
                        try_ray(origin, sdk_vec_normalize(sdk_vec_sub(target, origin)), std::abs(yaw) > 40.0);
                    }
                }
            }
        }
        out.failure = out.samples.empty() ? out.failure : "ok";
        return out;
    }

    auto sdk_side_back_metadata(const SdkAtlasSideBackResult& side) -> std::string
    {
        return ",\"side_back_attempts\":" + std::to_string(side.attempts) +
               ",\"side_back_success\":" + std::to_string(side.success) +
               ",\"side_back_owner_hits\":" + std::to_string(side.owner_hits) +
               ",\"side_back_uv_hits\":" + std::to_string(side.uv_hits) +
               ",\"side_hits\":" + std::to_string(side.side_hits) +
               ",\"back_hits\":" + std::to_string(side.back_hits) +
               ",\"side_back_duplicate_texels\":" + std::to_string(side.duplicate_texels) +
               ",\"side_back_nearest_sources\":" + std::to_string(side.nearest_sources) +
               ",\"side_back_failure\":\"" + json_escape(side.failure) + "\"";
    }

    auto sdk_assemble_texture_atlas(const ChannelBuffer& before_albedo,
                                    const std::vector<FrontSample>& samples) -> SdkTextureAtlas
    {
        SdkTextureAtlas out{};
        out.stats.width = before_albedo.width;
        out.stats.height = before_albedo.height;
        out.stats.source_samples = static_cast<int>(samples.size());
        const auto width = std::max(1, before_albedo.width);
        const auto height = std::max(1, before_albedo.height);
        const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (before_albedo.bytes.size() < pixels * 4)
        {
            out.failure = "atlas_albedo_bytes_invalid";
            return out;
        }
        struct Texel
        {
            double r{0.0};
            double g{0.0};
            double b{0.0};
            double weight{0.0};
            int priority{0};
            bool floor_like{false};
        };
        std::vector<Texel> texels(pixels);
        std::vector<std::uint8_t> direct_mask(pixels, 0);
        auto splat = [&](const FrontSample& sample) {
            const auto priority = sample.atlas_priority > 0 ? sample.atlas_priority : (sample.floor_like ? 12 : 11);
            const auto weight = sample.atlas_weight > 0.0 ? sample.atlas_weight : (sample.floor_like ? 88.0 : 72.0);
            const auto cx = std::max(0, std::min(width - 1, static_cast<int>(std::round(clamp01(sample.u) * static_cast<double>(width - 1)))));
            const auto cy = std::max(0, std::min(height - 1, static_cast<int>(std::round(clamp01(sample.v) * static_cast<double>(height - 1)))));
            const auto radius = std::max(0, sample.atlas_radius);
            for (int dy = -radius; dy <= radius; ++dy)
            {
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    if (dx * dx + dy * dy > radius * radius)
                    {
                        continue;
                    }
                    const auto x = cx + dx;
                    const auto y = cy + dy;
                    if (x < 0 || y < 0 || x >= width || y >= height)
                    {
                        continue;
                    }
                    const auto index = static_cast<std::size_t>(y * width + x);
                    auto& texel = texels[index];
                    if (priority < texel.priority)
                    {
                        continue;
                    }
                    if (priority > texel.priority)
                    {
                        texel = Texel{};
                        texel.priority = priority;
                    }
                    const auto dist_sq = static_cast<double>(dx * dx + dy * dy);
                    const auto radius_sq = static_cast<double>(std::max(1, radius) * std::max(1, radius));
                    const auto local_weight = radius <= 0
                        ? weight
                        : weight * std::max(0.15, std::min(1.0, 1.0 - dist_sq / (radius_sq + 1.0)));
                    texel.r += clamp01(sample.r) * local_weight;
                    texel.g += clamp01(sample.g) * local_weight;
                    texel.b += clamp01(sample.b) * local_weight;
                    texel.weight += local_weight;
                    texel.floor_like = texel.floor_like || sample.floor_like;
                    direct_mask[index] = 1;
                }
            }
        };
        for (const auto& sample : samples)
        {
            splat(sample);
        }
        for (const auto mask : direct_mask)
        {
            if (mask)
            {
                ++out.stats.direct_texels;
            }
        }

        std::vector<std::uint8_t> painted_mask = direct_mask;
        constexpr int fill_radius = 3;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const auto index = static_cast<std::size_t>(y * width + x);
                if (direct_mask[index])
                {
                    continue;
                }
                int best_distance = fill_radius * fill_radius + 1;
                int best_priority = -1;
                std::size_t best_index = static_cast<std::size_t>(-1);
                for (int dy = -fill_radius; dy <= fill_radius; ++dy)
                {
                    for (int dx = -fill_radius; dx <= fill_radius; ++dx)
                    {
                        const auto sx = x + dx;
                        const auto sy = y + dy;
                        const auto dist = dx * dx + dy * dy;
                        if (sx < 0 || sy < 0 || sx >= width || sy >= height || dist > fill_radius * fill_radius)
                        {
                            continue;
                        }
                        const auto source_index = static_cast<std::size_t>(sy * width + sx);
                        if (!direct_mask[source_index] || texels[source_index].weight <= 0.000001)
                        {
                            continue;
                        }
                        const auto priority = texels[source_index].priority;
                        if (dist < best_distance ||
                            (dist == best_distance && priority > best_priority) ||
                            (dist == best_distance && priority == best_priority && source_index < best_index))
                        {
                            best_distance = dist;
                            best_priority = priority;
                            best_index = source_index;
                        }
                    }
                }
                if (best_index == static_cast<std::size_t>(-1))
                {
                    continue;
                }
                texels[index] = texels[best_index];
                painted_mask[index] = 1;
                ++out.stats.filled_by_extension;
            }
        }

        out.albedo = before_albedo.bytes;
        out.painted_mask = std::move(painted_mask);
        for (std::size_t index = 0; index < pixels; ++index)
        {
            if (!out.painted_mask[index] || texels[index].weight <= 0.000001)
            {
                ++out.stats.preserved_original;
                continue;
            }
            const auto inv = 1.0 / texels[index].weight;
            const auto offset = index * 4;
            out.albedo[offset + 0] = sdk_byte_from_unit(std::max(0.02, std::min(0.98, texels[index].r * inv)));
            out.albedo[offset + 1] = sdk_byte_from_unit(std::max(0.02, std::min(0.98, texels[index].g * inv)));
            out.albedo[offset + 2] = sdk_byte_from_unit(std::max(0.02, std::min(0.98, texels[index].b * inv)));
            out.albedo[offset + 3] = before_albedo.bytes[offset + 3];
        }
        out.hash = hash_bytes(out.albedo);
        out.ok = out.stats.direct_texels > 0;
        out.failure = out.ok ? "ok" : "atlas_no_direct_texels";
        return out;
    }

    auto sdk_atlas_metadata(const SdkTextureAtlas& atlas) -> std::string
    {
        return ",\"atlas_source\":\"cpu_intermediate\"" +
               std::string(",\"albedo_width\":") + std::to_string(atlas.stats.width) +
               ",\"albedo_height\":" + std::to_string(atlas.stats.height) +
               ",\"srgb\":true" +
               ",\"direct_texels\":" + std::to_string(atlas.stats.direct_texels) +
               ",\"filled_by_extension\":" + std::to_string(atlas.stats.filled_by_extension) +
               ",\"preserved_original\":" + std::to_string(atlas.stats.preserved_original) +
               ",\"atlas_source_samples\":" + std::to_string(atlas.stats.source_samples) +
               ",\"atlas_hash\":\"" + std::to_string(atlas.hash) + "\"" +
               ",\"atlas_failure\":\"" + json_escape(atlas.failure) + "\"";
    }

    auto sdk_build_atlas_strokes(const SdkContext& ctx, const SdkTextureAtlas& atlas) -> SdkAtlasStrokePlan
    {
        SdkAtlasStrokePlan plan{};
        if (!atlas.ok || atlas.albedo.empty() || atlas.painted_mask.empty() || atlas.stats.width <= 0 || atlas.stats.height <= 0)
        {
            plan.failure = "atlas_unavailable";
            return plan;
        }
        const auto width = atlas.stats.width;
        const auto height = atlas.stats.height;
        const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        std::vector<std::uint8_t> used(pixels, 0);
        auto block_ok = [&](int x, int y, int size, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) -> bool {
            if (x + size > width || y + size > height)
            {
                return false;
            }
            int r_min = 255, g_min = 255, b_min = 255;
            int r_max = 0, g_max = 0, b_max = 0;
            bool any = false;
            for (int by = 0; by < size; ++by)
            {
                for (int bx = 0; bx < size; ++bx)
                {
                    const auto index = static_cast<std::size_t>((y + by) * width + (x + bx));
                    if (index >= pixels || used[index] || !atlas.painted_mask[index])
                    {
                        return false;
                    }
                    const auto offset = index * 4;
                    const int cr = atlas.albedo[offset + 0];
                    const int cg = atlas.albedo[offset + 1];
                    const int cb = atlas.albedo[offset + 2];
                    r_min = std::min(r_min, cr); r_max = std::max(r_max, cr);
                    g_min = std::min(g_min, cg); g_max = std::max(g_max, cg);
                    b_min = std::min(b_min, cb); b_max = std::max(b_max, cb);
                    any = true;
                }
            }
            if (!any || r_max - r_min > 2 || g_max - g_min > 2 || b_max - b_min > 2)
            {
                return false;
            }
            r = static_cast<std::uint8_t>((r_min + r_max) / 2);
            g = static_cast<std::uint8_t>((g_min + g_max) / 2);
            b = static_cast<std::uint8_t>((b_min + b_max) / 2);
            return true;
        };
        auto mark_used = [&](int x, int y, int size) {
            for (int by = 0; by < size; ++by)
            {
                for (int bx = 0; bx < size; ++bx)
                {
                    used[static_cast<std::size_t>((y + by) * width + (x + bx))] = 1;
                }
            }
        };
        auto push_stroke = [&](int x, int y, int size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            const auto radius_uv = std::max(1.0 / static_cast<double>(std::max(width, height)),
                                            (static_cast<double>(size) * 0.62) / static_cast<double>(std::max(width, height)));
            auto brush = sdk_copy_current_brush(ctx, radius_uv);
            const auto channel = sdk_make_channel(static_cast<double>(r) / 255.0,
                                                  static_cast<double>(g) / 255.0,
                                                  static_cast<double>(b) / 255.0,
                                                  0.0,
                                                  0.65,
                                                  meccha_sdk::EPaintChannelApplyMode::Override);
            const auto u = (static_cast<double>(x) + static_cast<double>(size) * 0.5) / static_cast<double>(width);
            const auto v = (static_cast<double>(y) + static_cast<double>(size) * 0.5) / static_cast<double>(height);
            plan.strokes.push_back(sdk_make_uv_stroke(u, v, channel, brush, meccha_sdk::EPaintChannel::Albedo));
            if (size >= 8) ++plan.merged_8;
            else if (size >= 4) ++plan.merged_4;
            else if (size >= 2) ++plan.merged_2;
            else ++plan.singles;
        };
        const int block_sizes[]{8, 4, 2, 1};
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const auto index = static_cast<std::size_t>(y * width + x);
                if (used[index] || !atlas.painted_mask[index])
                {
                    continue;
                }
                ++plan.source_texels;
                bool emitted = false;
                for (const auto size : block_sizes)
                {
                    std::uint8_t r = 0, g = 0, b = 0;
                    if (block_ok(x, y, size, r, g, b))
                    {
                        push_stroke(x, y, size, r, g, b);
                        mark_used(x, y, size);
                        emitted = true;
                        break;
                    }
                }
                if (!emitted)
                {
                    const auto offset = index * 4;
                    push_stroke(x, y, 1, atlas.albedo[offset + 0], atlas.albedo[offset + 1], atlas.albedo[offset + 2]);
                    used[index] = 1;
                }
                if (static_cast<int>(plan.strokes.size()) > plan.stroke_cap)
                {
                    plan.cap_exceeded = true;
                    plan.failure = "atlas_stroke_budget_exceeded";
                    return plan;
                }
            }
        }
        plan.failure = plan.strokes.empty() ? "atlas_strokes_empty" : "ok";
        return plan;
    }

    auto sdk_atlas_stroke_metadata(const SdkAtlasStrokePlan& plan) -> std::string
    {
        return ",\"stroke_count\":" + std::to_string(plan.strokes.size()) +
               ",\"stroke_cap\":" + std::to_string(plan.stroke_cap) +
               ",\"stroke_target\":" + std::to_string(plan.target_strokes) +
               ",\"stroke_source_texels\":" + std::to_string(plan.source_texels) +
               ",\"stroke_merged_8\":" + std::to_string(plan.merged_8) +
               ",\"stroke_merged_4\":" + std::to_string(plan.merged_4) +
               ",\"stroke_merged_2\":" + std::to_string(plan.merged_2) +
               ",\"stroke_singles\":" + std::to_string(plan.singles) +
               ",\"merge_error_max\":" + std::to_string(plan.merge_error_max) +
               ",\"atlas_stroke_cap_exceeded\":" + std::string(json_bool(plan.cap_exceeded)) +
               ",\"atlas_stroke_failure\":\"" + json_escape(plan.failure) + "\"";
    }

    auto sdk_append_uv_gap_fill(std::vector<FrontSample>& samples, double brush_radius, SdkUvGapFillStats& stats) -> void
    {
        if (samples.empty())
        {
            return;
        }
        struct Cell
        {
            bool covered{false};
            bool direct{false};
            FrontSample sample{};
        };
        const auto cell_size = std::max(0.000001, brush_radius * 0.5);
        const auto grid_edge = std::max(8, std::min(512, static_cast<int>(std::ceil(1.0 / cell_size))));
        const auto actual_cell = 1.0 / static_cast<double>(grid_edge);
        const auto footprint = std::max(0, static_cast<int>(std::floor(brush_radius / actual_cell)));
        std::vector<Cell> cells(static_cast<std::size_t>(grid_edge * grid_edge));
        auto index_for = [grid_edge](int x, int y) { return static_cast<std::size_t>(y * grid_edge + x); };
        auto cell_for = [grid_edge](double value) {
            return std::max(0, std::min(grid_edge - 1, static_cast<int>(std::floor(clamp01(value) * static_cast<double>(grid_edge)))));
        };
        int min_x = grid_edge - 1;
        int min_y = grid_edge - 1;
        int max_x = 0;
        int max_y = 0;
        for (const auto& sample : samples)
        {
            const auto cx = cell_for(sample.u);
            const auto cy = cell_for(sample.v);
            min_x = std::min(min_x, cx);
            min_y = std::min(min_y, cy);
            max_x = std::max(max_x, cx);
            max_y = std::max(max_y, cy);
            for (int dy = -footprint; dy <= footprint; ++dy)
            {
                for (int dx = -footprint; dx <= footprint; ++dx)
                {
                    if (footprint > 0 && dx * dx + dy * dy > footprint * footprint)
                    {
                        continue;
                    }
                    const auto x = cx + dx;
                    const auto y = cy + dy;
                    if (x < 0 || y < 0 || x >= grid_edge || y >= grid_edge)
                    {
                        continue;
                    }
                    auto& cell = cells[index_for(x, y)];
                    cell.covered = true;
                    cell.direct = true;
                    cell.sample = sample;
                }
            }
        }
        const auto margin = footprint + 4;
        min_x = std::max(0, min_x - margin);
        min_y = std::max(0, min_y - margin);
        max_x = std::min(grid_edge - 1, max_x + margin);
        max_y = std::min(grid_edge - 1, max_y + margin);
        stats.considered_cells = std::max(1, (max_x - min_x + 1) * (max_y - min_y + 1));
        auto covered_count = [&]() {
            int count = 0;
            for (int y = min_y; y <= max_y; ++y)
            {
                for (int x = min_x; x <= max_x; ++x)
                {
                    if (cells[index_for(x, y)].covered)
                    {
                        ++count;
                    }
                }
            }
            return count;
        };
        stats.direct_cells = covered_count();
        stats.coverage_before = static_cast<double>(stats.direct_cells) / static_cast<double>(stats.considered_cells);
        auto source_at = [&](int x, int y, int dx, int dy, FrontSample& out, int& distance) {
            for (int step = 1; step <= 5; ++step)
            {
                const auto sx = x + dx * step;
                const auto sy = y + dy * step;
                if (sx < 0 || sy < 0 || sx >= grid_edge || sy >= grid_edge)
                {
                    continue;
                }
                const auto& cell = cells[index_for(sx, sy)];
                if (cell.direct)
                {
                    out = cell.sample;
                    distance = step;
                    return true;
                }
            }
            return false;
        };
        auto normal_dot = [](const FrontSample& a, const FrontSample& b) {
            return a.normal.X * b.normal.X + a.normal.Y * b.normal.Y + a.normal.Z * b.normal.Z;
        };
        const auto max_fill = std::min(4096, static_cast<int>(samples.size()) * 3);
        std::vector<FrontSample> fill{};
        fill.reserve(static_cast<std::size_t>(max_fill));
        for (int y = min_y; y <= max_y && static_cast<int>(fill.size()) < max_fill; ++y)
        {
            for (int x = min_x; x <= max_x && static_cast<int>(fill.size()) < max_fill; ++x)
            {
                if (cells[index_for(x, y)].covered)
                {
                    ++stats.rejected_occupied;
                    continue;
                }
                FrontSample left{}, right{}, up{}, down{};
                int dl = 0, dr = 0, du = 0, dd = 0;
                const bool has_lr = source_at(x, y, -1, 0, left, dl) && source_at(x, y, 1, 0, right, dr);
                const bool has_ud = source_at(x, y, 0, -1, up, du) && source_at(x, y, 0, 1, down, dd);
                if (!has_lr && !has_ud)
                {
                    ++stats.rejected_unbounded;
                    continue;
                }
                std::vector<FrontSample> sources{};
                if (has_lr)
                {
                    sources.push_back(left);
                    sources.push_back(right);
                }
                if (has_ud)
                {
                    sources.push_back(up);
                    sources.push_back(down);
                }
                bool normal_ok = true;
                for (std::size_t i = 0; i < sources.size() && normal_ok; ++i)
                {
                    for (std::size_t j = i + 1; j < sources.size(); ++j)
                    {
                        if (normal_dot(sources[i], sources[j]) < 0.62)
                        {
                            normal_ok = false;
                            break;
                        }
                    }
                }
                ++stats.candidates;
                if (!normal_ok)
                {
                    ++stats.rejected_normal;
                    continue;
                }
                FrontSample out{};
                out.u = (static_cast<double>(x) + 0.5) * actual_cell;
                out.v = (static_cast<double>(y) + 0.5) * actual_cell;
                out.radius = brush_radius;
                out.has_world_position = true;
                const auto inv = 1.0 / static_cast<double>(sources.size());
                for (const auto& source : sources)
                {
                    out.r += source.r * inv;
                    out.g += source.g * inv;
                    out.b += source.b * inv;
                    out.roughness += source.roughness * inv;
                    out.metallic += source.metallic * inv;
                    out.world_position.X += source.world_position.X * inv;
                    out.world_position.Y += source.world_position.Y * inv;
                    out.world_position.Z += source.world_position.Z * inv;
                    out.normal.X += source.normal.X * inv;
                    out.normal.Y += source.normal.Y * inv;
                    out.normal.Z += source.normal.Z * inv;
                }
                out.normal = sdk_vec_normalize(out.normal);
                fill.push_back(out);
                cells[index_for(x, y)].covered = true;
                ++stats.bounded;
            }
        }
        stats.sent = static_cast<int>(fill.size());
        stats.coverage_after = static_cast<double>(covered_count()) / static_cast<double>(stats.considered_cells);
        samples.insert(samples.end(), fill.begin(), fill.end());
    }

    auto sdk_gap_fill_metadata(const SdkUvGapFillStats& stats) -> std::string
    {
        return ",\"uv_gap_fill_candidates\":" + std::to_string(stats.candidates) +
               ",\"uv_gap_fill_sent\":" + std::to_string(stats.sent) +
               ",\"uv_gap_fill_bounded\":" + std::to_string(stats.bounded) +
               ",\"uv_gap_fill_edge_extended\":" + std::to_string(stats.edge_extended) +
               ",\"uv_gap_fill_rejected_unbounded\":" + std::to_string(stats.rejected_unbounded) +
               ",\"uv_gap_fill_rejected_normal\":" + std::to_string(stats.rejected_normal) +
               ",\"uv_gap_fill_rejected_occupied\":" + std::to_string(stats.rejected_occupied) +
               ",\"uv_gap_fill_direct_cells\":" + std::to_string(stats.direct_cells) +
               ",\"uv_gap_fill_considered_cells\":" + std::to_string(stats.considered_cells) +
               ",\"uv_gap_fill_coverage_before\":" + std::to_string(stats.coverage_before) +
               ",\"uv_gap_fill_coverage_after\":" + std::to_string(stats.coverage_after);
    }

    auto sdk_front_world_position_count(const std::vector<FrontSample>& samples) -> int
    {
        int count = 0;
        for (const auto& sample : samples)
        {
            if (sample.has_world_position)
            {
                ++count;
            }
        }
        return count;
    }

    auto sdk_build_metallic_base_strokes(const SdkContext& ctx) -> std::vector<meccha_sdk::FPaintStroke>
    {
        constexpr int grid = 32;
        constexpr double radius = 0.04;
        const auto brush = sdk_copy_current_brush(ctx, radius);
        const auto channel = sdk_make_channel(1.0,
                                              1.0,
                                              1.0,
                                              1.0,
                                              0.0,
                                              meccha_sdk::EPaintChannelApplyMode::Override);
        std::vector<meccha_sdk::FPaintStroke> strokes{};
        strokes.reserve(grid * grid);
        for (int y = 0; y < grid; ++y)
        {
            for (int x = 0; x < grid; ++x)
            {
                strokes.push_back(sdk_make_stroke((static_cast<double>(x) + 0.5) / static_cast<double>(grid),
                                                  (static_cast<double>(y) + 0.5) / static_cast<double>(grid),
                                                  channel,
                                                  brush,
                                                  meccha_sdk::EPaintChannel::AlbedoMetallicRoughness,
                                                  ctx.body_world_position));
            }
        }
        return strokes;
    }

    auto sdk_build_front_strokes(const SdkContext& ctx, const std::vector<FrontSample>& samples) -> std::vector<meccha_sdk::FPaintStroke>
    {
        std::vector<meccha_sdk::FPaintStroke> strokes{};
        strokes.reserve(samples.size());
        for (const auto& sample : samples)
        {
            const auto brush = sdk_copy_current_brush(ctx, sample.radius);
            const auto channel = sdk_make_channel(sample.r,
                                                  sample.g,
                                                  sample.b,
                                                  0.0,
                                                  0.65,
                                                  meccha_sdk::EPaintChannelApplyMode::AlphaBlend);
            const auto world_position = sample.has_world_position ? sample.world_position : ctx.body_world_position;
            strokes.push_back(sdk_make_stroke(sample.u,
                                              sample.v,
                                              channel,
                                              brush,
                                              meccha_sdk::EPaintChannel::AlbedoMetallicRoughness,
                                              world_position));
        }
        return strokes;
    }

    auto sdk_colorize_native_front_samples(const std::vector<FrontSample>& native_surface_samples,
                                           const std::vector<FrontSample>& payload_samples)
        -> std::vector<FrontSample>
    {
        std::vector<FrontSample> samples{};
        if (native_surface_samples.empty())
        {
            return samples;
        }
        samples.reserve(native_surface_samples.size());
        for (const auto& surface : native_surface_samples)
        {
            FrontSample sample = surface;
            const FrontSample* nearest_payload = nullptr;
            double best_distance = 1000000.0;
            for (const auto& payload : payload_samples)
            {
                const auto du = clamp01(payload.u) - clamp01(surface.u);
                const auto dv = clamp01(payload.v) - clamp01(surface.v);
                const auto distance = du * du + dv * dv;
                if (!nearest_payload || distance < best_distance)
                {
                    nearest_payload = &payload;
                    best_distance = distance;
                }
            }
            if (nearest_payload)
            {
                sample.r = clamp01(nearest_payload->r);
                sample.g = clamp01(nearest_payload->g);
                sample.b = clamp01(nearest_payload->b);
                sample.radius = std::max(0.012, std::min(0.035, nearest_payload->radius > 0.0 ? nearest_payload->radius : 0.018));
            }
            else
            {
                sample.r = 0.34;
                sample.g = 0.36;
                sample.b = 0.32;
                sample.radius = 0.018;
            }
            sample.u = clamp01(surface.u);
            sample.v = clamp01(surface.v);
            sample.roughness = 0.65;
            sample.has_world_position = true;
            sample.world_position = surface.world_position;
            samples.push_back(sample);
        }
        return samples;
    }

    auto sdk_paint_full_route_native_direct(const std::string& request) -> std::string
    {
        const auto start = std::chrono::steady_clock::now();
        const bool is_probe = request.find("\"type\":\"sdk_probe\"") != std::string::npos;
        const bool diagnostic_import = request.find("\"native_apply_mode\":\"texture_import_diagnostic\"") != std::string::npos ||
                                       request.find("\"route\":\"f10_texture_import_diagnostic\"") != std::string::npos;
        static volatile LONG paint_busy = 0;
        struct BusyGuard
        {
            volatile LONG* flag{nullptr};
            bool active{false};
            ~BusyGuard()
            {
                if (active && flag)
                {
                    InterlockedExchange(flag, 0);
                }
            }
        } busy_guard{&paint_busy, false};
        if (!is_probe)
        {
            if (InterlockedCompareExchange(&paint_busy, 1, 0) != 0)
            {
                return response_json(false, "paint_ignored_busy", 0, 1, "paint request ignored because another paint is in progress");
            }
            busy_guard.active = true;
        }
        Reflection ref{};
        std::string failure{};
        if (!ref.init(failure))
        {
            return response_json(false, "sdk_unavailable", 0, 1, failure);
        }
        const auto ctx = sdk_resolve_context(ref);
        std::string metadata = sdk_context_metadata(ref, ctx);
        if (!ctx.ok)
        {
            const auto stage = is_probe ? ctx.stage : std::string("sdk_context_unavailable");
            return response_json(false, stage.c_str(), 0, 1, ctx.message, metadata);
        }
        if (!diagnostic_import && !sdk_has_replicated_api(ctx))
        {
            return response_json(false,
                                 "replicated_api_unavailable",
                                 0,
                                 1,
                                 "ServerPaintBatch replicated paint RPC is unavailable",
                                 metadata + ",\"bridge_events\":[\"replicated_api_unavailable\"]");
        }

        metadata += ",\"render_target_albedo\":\"" + hex_address(sdk_get_render_target(ctx, 0)) + "\"" +
                    ",\"route\":\"" + std::string(diagnostic_import ? "sdk_texture_import_diagnostic" : "sdk_texture_atlas_replicated_strokes") + "\"" +
                    ",\"replication\":\"component_server_paint_batch\"" +
                    ",\"replicated_paint_used\":" + json_bool(!diagnostic_import) +
                    ",\"temporary_diagnostic_only\":" + json_bool(diagnostic_import) +
                    ",\"diagnostic_import_channels\":[\"albedo\"]" +
                    ",\"front_payload_placement_used\":false" +
                    ",\"front_payload_color_used\":false" +
                    ",\"metallic_base_skipped\":true" +
                    ",\"target_channel\":\"Albedo\"";

        auto before0 = sdk_export_channel_bytes(ref, ctx, 0);
        metadata += sdk_channel_metadata("before_albedo", before0);

        if (!before0.ok)
        {
            return response_json(false,
                                 "export_failed",
                                 0,
                                 1,
                                 "SDK albedo export failed: " + before0.failure,
                                 metadata);
        }
        if (is_probe)
        {
            return response_json(true,
                                 "sdk_probe",
                                 0,
                                 0,
                                 "SDK probe ok; replicated paint API ready",
                                 metadata + ",\"bridge_events\":[\"replicated_api_ready\"]");
        }

        SdkNativeFrontSampleResult native_front{};
        SdkFrontCaptureProbe front_capture{};
        SdkReplicatedStats atlas_stats{};
        std::string bridge_events = "\"bridge_events\":[\"replicated_api_ready\",\"atlas_sampling_started\"";
        if (!kEnableNativeSceneCaptureForF10)
        {
            metadata += std::string(",\"front_capture_backend_enabled\":false") +
                        ",\"front_capture_failure\":\"front_capture_backend_disabled_after_d3d12_crash\"" +
                        ",\"stroke_count\":0" +
                        "," + bridge_events + ",\"atlas_capture_unavailable\"]";
            return response_json(false,
                                 "atlas_capture_unavailable",
                                 0,
                                 1,
                                 "native SceneCapture2D/CreateRenderTarget2D backend disabled; no paint was dispatched",
                                 metadata);
        }
        native_front = sdk_collect_native_front_samples(ref, ctx, {});
        metadata += sdk_native_front_metadata(native_front) +
                    ",\"front_bbox\":\"" + std::to_string(native_front.bbox_min_nx) + "," + std::to_string(native_front.bbox_min_ny) +
                    "-" + std::to_string(native_front.bbox_max_nx) + "," + std::to_string(native_front.bbox_max_ny) + "\"" +
                    ",\"viewport\":\"" + std::to_string(native_front.viewport_width) + "x" + std::to_string(native_front.viewport_height) + "\"" +
                    ",\"capture_fov_hint\":\"deproject_horizontal\"";
        if (static_cast<int>(native_front.samples.size()) < native_front.min_front_hits)
        {
            metadata += std::string(",\"stroke_count\":0") +
                        "," + bridge_events + ",\"atlas_sampling_insufficient\"]";
            return response_json(false,
                                 "atlas_sampling_insufficient",
                                 0,
                                 1,
                                 "native surface samples are insufficient; no paint was dispatched",
                                 metadata);
        }

        bridge_events += ",\"atlas_sampling_done\",\"atlas_capture_started\"";
        front_capture = sdk_probe_front_capture_backend(ref, ctx, native_front);
        metadata += sdk_front_capture_metadata(front_capture);
        auto captured_front = sdk_capture_front_colors(ref, ctx, native_front, before0.width, before0.height);
        metadata += sdk_capture_metadata(captured_front) +
                    ",\"front_capture_resolution_source\":\"paint_channel_export\"" +
                    ",\"front_capture_target_width\":" + std::to_string(before0.width) +
                    ",\"front_capture_target_height\":" + std::to_string(before0.height);
        if (!captured_front.ok)
        {
            metadata += std::string(",\"stroke_count\":0") +
                        "," + bridge_events + ",\"atlas_capture_unavailable\"]";
            return response_json(false,
                                 "atlas_capture_unavailable",
                                 0,
                                 1,
                                 "front capture/readback failed; no paint was dispatched: " + captured_front.failure,
                                 metadata);
        }
        bridge_events += ",\"atlas_capture_done\"";

        auto side_back = sdk_collect_atlas_side_back_samples(ref, ctx, native_front, captured_front.samples, before0.width, before0.height);
        metadata += sdk_side_back_metadata(side_back);
        constexpr int min_side_back_hits = 64;
        if (!diagnostic_import && static_cast<int>(side_back.samples.size()) < min_side_back_hits)
        {
            metadata += ",\"front_hits\":" + std::to_string(captured_front.samples.size()) +
                        ",\"stroke_count\":0" +
                        "," + bridge_events + ",\"atlas_side_back_insufficient\"]";
            return response_json(false,
                                 "atlas_side_back_insufficient",
                                 0,
                                 1,
                                 "side/back samples are insufficient; no paint was dispatched",
                                 metadata);
        }
        bridge_events += ",\"atlas_side_back_done\"";

        std::vector<FrontSample> atlas_samples = captured_front.samples;
        atlas_samples.insert(atlas_samples.end(), side_back.samples.begin(), side_back.samples.end());
        auto atlas = sdk_assemble_texture_atlas(before0, atlas_samples);
        metadata += sdk_atlas_metadata(atlas) +
                    ",\"front_hits\":" + std::to_string(captured_front.samples.size()) +
                    ",\"side_back_hits\":" + std::to_string(side_back.samples.size());
        if (!atlas.ok || atlas.stats.direct_texels < 2048)
        {
            metadata += std::string(",\"stroke_count\":0") +
                        "," + bridge_events + ",\"atlas_quality_failed\"]";
            return response_json(false,
                                 "atlas_quality_failed",
                                 0,
                                 1,
                                 "atlas assembly quality failed: " + atlas.failure,
                                 metadata);
        }
        bridge_events += ",\"atlas_assembled\"";

        if (diagnostic_import)
        {
            bridge_events += ",\"diagnostic_import_started\"";
            std::string import_failure{};
            const bool import_ok = sdk_import_channel_bytes(ctx, 0, atlas.albedo, import_failure);
            Sleep(100);
            auto after0 = sdk_export_channel_bytes(ref, ctx, 0);
            const bool hash_changed = after0.ok && after0.hash != before0.hash;
            const bool import_observed = import_ok && after0.ok && after0.hash == atlas.hash && hash_changed;
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            metadata += sdk_channel_metadata("after_albedo", after0) +
                        ",\"atlas_hash\":\"" + std::to_string(atlas.hash) + "\"" +
                        ",\"before_albedo_hash\":\"" + std::to_string(before0.hash) + "\"" +
                        ",\"after_albedo_hash\":\"" + std::to_string(after0.hash) + "\"" +
                        ",\"hash_changed\":" + json_bool(hash_changed) +
                        ",\"diagnostic_import_ok\":" + json_bool(import_ok) +
                        ",\"diagnostic_import_failure\":\"" + json_escape(import_failure) + "\"" +
                        ",\"diagnostic_import_observed\":" + json_bool(import_observed) +
                        ",\"server_rpc\":\"none\"" +
                        ",\"server_batches\":0" +
                        ",\"server_sent\":0" +
                        ",\"server_failed\":0" +
                        ",\"stroke_count\":0" +
                        ",\"elapsed_ms\":" + std::to_string(elapsed);

            if (!import_ok)
            {
                metadata += "," + bridge_events + ",\"diagnostic_import_failed\"]";
                return response_json(false,
                                     "diagnostic_import_failed",
                                     0,
                                     1,
                                     "diagnostic albedo import failed: " + import_failure,
                                     metadata);
            }
            if (!import_observed)
            {
                metadata += "," + bridge_events + ",\"diagnostic_import_not_observed\"]";
                return response_json(false,
                                     "diagnostic_import_not_observed",
                                     0,
                                     1,
                                     "diagnostic albedo import was not observed by export/hash verification",
                                     metadata);
            }

            metadata += "," + bridge_events + ",\"diagnostic_import_done\"]";
            return response_json(true,
                                 "diagnostic_import_done",
                                 1,
                                 0,
                                 "temporary diagnostic albedo import observed",
                                 metadata);
        }

        auto stroke_plan = sdk_build_atlas_strokes(ctx, atlas);
        metadata += sdk_atlas_stroke_metadata(stroke_plan);
        if (stroke_plan.cap_exceeded || stroke_plan.strokes.empty() || static_cast<int>(stroke_plan.strokes.size()) > stroke_plan.stroke_cap)
        {
            metadata += "," + bridge_events + ",\"atlas_stroke_budget_exceeded\"]";
            return response_json(false,
                                 "atlas_stroke_budget_exceeded",
                                 0,
                                 1,
                                 "atlas stroke generation exceeded the hard cap; no paint was dispatched",
                                 metadata);
        }
        bridge_events += ",\"atlas_strokes_generated\",\"replicated_stream_started\"";

        if (!sdk_dispatch_replicated_strokes(ctx, stroke_plan.strokes, atlas_stats))
        {
            metadata += sdk_stats_metadata("atlas_paint", atlas_stats) +
                        "," + bridge_events + ",\"server_batch_failed\"]";
            return response_json(false,
                                 "server_batch_failed",
                                 atlas_stats.server_sent,
                                 std::max(1, atlas_stats.server_failed),
                                 "atlas paint ServerPaintBatch failed: " + atlas_stats.first_failure,
                                 metadata);
        }
        bridge_events += ",\"replicated_stream_done\"";
        Sleep(100);

        auto after0 = sdk_export_channel_bytes(ref, ctx, 0);
        const bool hash_changed = after0.ok && after0.hash != before0.hash;
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        metadata += sdk_stats_metadata("atlas_paint", atlas_stats) +
                    sdk_channel_metadata("after_albedo", after0) +
                    ",\"hash_before\":\"" + std::to_string(before0.hash) + "\"" +
                    ",\"hash_after\":\"" + std::to_string(after0.hash) + "\"" +
                    ",\"hash_changed\":" + json_bool(hash_changed) +
                    ",\"server_rpc\":\"ServerPaintBatch\"" +
                    ",\"server_batches\":" + std::to_string(atlas_stats.batch_calls) +
                    ",\"server_sent\":" + std::to_string(atlas_stats.server_sent) +
                    ",\"server_failed\":" + std::to_string(atlas_stats.server_failed) +
                    ",\"elapsed_ms\":" + std::to_string(elapsed);

        if (!hash_changed)
        {
            metadata += "," + bridge_events + ",\"hash_unchanged\"]";
            return response_json(false,
                                 "hash_unchanged",
                                 atlas_stats.server_sent,
                                 1,
                                 "atlas paint server RPC dispatched, but albedo hash did not change",
                                 metadata);
        }

        metadata += "," + bridge_events + ",\"paint_done\"]";
        return response_json(true,
                             "paint_done",
                             atlas_stats.server_sent,
                             0,
                             "texture-atlas sourced replicated paint applied",
                             metadata);
    }

    auto paint_full_route_native_direct(const std::string& request) -> std::string
    {
        return sdk_paint_full_route_native_direct(request);
    }

    auto drain_paint_jobs_on_game_thread() -> void
    {
        std::vector<std::shared_ptr<QueuedPaintJob>> jobs{};
        {
            std::lock_guard<std::mutex> lock(g_paint_jobs_mutex);
            jobs.swap(g_paint_jobs);
        }
        for (const auto& job : jobs)
        {
            if (!job)
            {
                continue;
            }
            const auto response = paint_full_route_native_direct(job->request);
            {
                std::lock_guard<std::mutex> lock(g_paint_jobs_mutex);
                job->response = response;
                job->done = true;
            }
            g_paint_jobs_cv.notify_all();
        }
    }

    void __fastcall hooked_process_event(void* object, void* function, void* params)
    {
        const auto original = g_original_process_event.load();
        if (!g_inside_process_event_hook)
        {
            g_inside_process_event_hook = true;
            __try
            {
                drain_paint_jobs_on_game_thread();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            g_inside_process_event_hook = false;
        }
        if (original)
        {
            reinterpret_cast<ProcessEventFn>(original)(object, function, params);
        }
    }

    LRESULT CALLBACK message_hook_proc(int code, WPARAM wparam, LPARAM lparam)
    {
        if (code >= 0)
        {
            __try
            {
                drain_paint_jobs_on_game_thread();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return CallNextHookEx(g_message_hook.load(), code, wparam, lparam);
    }

    auto paint_full_route_native(const std::string& request) -> std::string
    {
        std::string failure{};
        if (!install_process_event_hook(failure))
        {
            return response_json(false, failure.c_str(), 0, 1, failure);
        }
        auto job = std::make_shared<QueuedPaintJob>();
        job->request = request;
        {
            std::lock_guard<std::mutex> lock(g_paint_jobs_mutex);
            g_paint_jobs.push_back(job);
        }
        g_paint_jobs_cv.notify_all();
        if (const auto thread_id = g_game_thread_id.load())
        {
            PostThreadMessageW(thread_id, PaintDispatchMessage, 0, 0);
        }
        std::unique_lock<std::mutex> lock(g_paint_jobs_mutex);
        const bool completed = g_paint_jobs_cv.wait_for(lock, std::chrono::seconds(240), [&]() {
            return job->done;
        });
        if (!completed)
        {
            return response_json(false, "game_thread_dispatch_timeout", 0, 1, "game thread did not process paint job");
        }
        return job->response;
    }

    auto handle_request(const std::string& line) -> std::string
    {
        if (line.find("\"type\":\"ping\"") != std::string::npos)
        {
            return response_json(true, "ping", 0, 0, "pong");
        }
        if (line.find("\"type\":\"capabilities\"") != std::string::npos)
        {
            return "{\"success\":true,\"stage\":\"capabilities\",\"applied\":0,\"failures\":0,"
                   "\"message\":\"ok\",\"timing_ms\":{},"
                   "\"metadata\":{\"commands\":[\"ping\",\"capabilities\",\"sdk_probe\",\"paint_full_route\",\"shutdown\"],"
                   "\"sdk\":\"chameleonEsp_dumper7_1_7_0_min\","
                   "\"paint_full_route\":\"sdk_texture_atlas_replicated_strokes\","
                   "\"replication\":\"component_server_paint_batch\","
                   "\"multiplayer_replicated\":true}}\n";
        }
        if (line.find("\"type\":\"shutdown\"") != std::string::npos)
        {
            uninstall_process_event_hook();
            g_running.store(false);
            return response_json(true, "shutdown", 0, 0, "bridge shutdown requested");
        }
        if (line.find("\"type\":\"sdk_probe\"") != std::string::npos)
        {
            return paint_full_route_native(line);
        }
        if (line.find("\"type\":\"paint_full_route\"") != std::string::npos)
        {
            return paint_full_route_native(line);
        }
        return response_json(false, "unknown_command", 0, 1, "unknown bridge command");
    }

    auto bridge_thread() -> void
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            return;
        }
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            WSACleanup();
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<u_short>(resolve_bridge_port()));
        const int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listener, 4) == SOCKET_ERROR)
        {
            closesocket(listener);
            WSACleanup();
            return;
        }
        auto last_activity = std::chrono::steady_clock::now();
        while (g_running.load())
        {
            fd_set read_set{};
            FD_ZERO(&read_set);
            FD_SET(listener, &read_set);
            timeval timeout{};
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
            if (selected == SOCKET_ERROR)
            {
                break;
            }
            if (selected == 0)
            {
                const auto idle_seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_activity).count();
                if (!g_process_event_hook_installed.load() && idle_seconds >= IdleShutdownSeconds)
                {
                    break;
                }
                continue;
            }
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                continue;
            }
            last_activity = std::chrono::steady_clock::now();
            const int timeout_ms = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            std::string request{};
            request.reserve(65536);
            char buffer[16384]{};
            while (request.size() < MaxRequestBytes)
            {
                const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
                if (received <= 0)
                {
                    break;
                }
                request.append(buffer, static_cast<std::size_t>(received));
                if (request.find('\n') != std::string::npos)
                {
                    break;
                }
            }
            if (!request.empty())
            {
                const std::string response = request.size() >= MaxRequestBytes
                                                 ? response_json(false, "request_too_large", 0, 1, "bridge request exceeded max size")
                                                 : handle_request(request);
                send(client, response.c_str(), static_cast<int>(response.size()), 0);
            }
            closesocket(client);
        }
        closesocket(listener);
        WSACleanup();
        uninstall_process_event_hook();
        HMODULE module = g_module;
        g_module = nullptr;
        if (module != nullptr)
        {
            FreeLibraryAndExitThread(module, 0);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_module = module;
        std::thread(bridge_thread).detach();
    }
    if (reason == DLL_PROCESS_DETACH)
    {
        g_running.store(false);
    }
    return TRUE;
}
