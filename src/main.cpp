#if defined(_WIN32)
#include <Windows.h>
#else
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "cart_rom_mirror.hpp"
#include "common/rt64_performance_profiler.h"
#include "funcs.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "native_boot_overlays.hpp"
#include "native_checkpoint_bridge.hpp"
#include "native_death_screen.hpp"
#include "native_electric_effect.hpp"
#include "native_controller_pak.hpp"
#include "native_game_completion_screen.hpp"
#include "native_first_run.hpp"
#include "native_first_run_assets.hpp"
#include "native_graphics_options.hpp"
#include "native_input_bindings.hpp"
#include "native_level_editor.hpp"
#include "native_menu_background.hpp"
#include "native_modern_controls.hpp"
#include "native_modern_sky.hpp"
#include "native_object_cull_telemetry.hpp"
#include "native_rt64_renderer.hpp"
#include "native_rsp_task_bridge.hpp"
#include "native_sdl_io.hpp"
#include "native_startup_flow.hpp"
#include "native_text_overlay.hpp"
#include "native_weapon_system.hpp"
#include "native_windows_portable.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "../lib/RT64/src/common/rt64_performance_profiler.h"
#include "ultramodern/rt64_guest_vi_bridge.hpp"

extern RspUcodeFunc n_aspMain;

namespace {

constexpr uint64_t kRomHash = 0x06BDD92F3F4AE7BDULL;
const std::u8string kGameId = u8"buck-bumble-us-rev0";
constexpr gpr kEntrypointAddress =
    static_cast<gpr>(static_cast<int32_t>(0x80010400u));
constexpr int32_t kLoadedCoreAlias = static_cast<int32_t>(0x8002F9B0u);
constexpr uint32_t kLoadedCoreRom = 0x000205B0u;
constexpr int32_t kLoadedCoreRam = static_cast<int32_t>(0x80043970u);
constexpr uint32_t kLoadedCoreSize = 0x000BCE50u;
constexpr uint32_t kInitialBssStart = 0x8002F9B0u;
constexpr uint32_t kInitialBssEnd = 0x80043970u;
constexpr gpr kInitialStack =
    static_cast<gpr>(static_cast<int32_t>(0x803FFFF0u));
#if defined(_WIN32)
constexpr wchar_t kWindowClassName[] = L"BumbleWindow";
constexpr UINT kValidationSyntheticMouseMessage = WM_APP + 0x42u;
constexpr UINT kValidationSyntheticKeyMessage = WM_APP + 0x43u;
constexpr UINT kValidationSyntheticMouseButtonMessage = WM_APP + 0x44u;
// 0x45 belongs to graphics-options window resizing.
constexpr UINT kValidationSyntheticWheelMessage = WM_APP + 0x46u;
#endif

std::atomic_uint32_t g_audio_tasks{0};
std::atomic_bool g_game_start_requested{false};
std::atomic_bool g_replay_close_requested{false};
#if defined(_WIN32)
std::atomic_bool g_window_close_requested{false};
std::atomic_bool g_window_close_deferred_logged{false};
HWND g_window = nullptr;
#else
SDL_Window* g_window = nullptr;
#endif
bool g_modern_controls_requested = false;
bool g_window_focused = false;
bool g_validation_synthetic_input = false;
uint64_t g_mouse_clip_reassert_count = 0;
#if defined(_WIN32)
HANDLE g_single_instance_mutex = nullptr;
#else
int g_single_instance_fd = -1;
#endif

void profile_vi_tick(
    uint64_t wake_lateness_nanoseconds,
    uint64_t skipped_retraces
) {
    RT64::PerformanceProfiler::recordRuntimeVI(
        wake_lateness_nanoseconds, skipped_retraces, true);
}

void profile_graphics_action_dequeued(
    ultramodern::events::GraphicsActionType type,
    uint64_t queued_nanoseconds,
    uint64_t dequeued_nanoseconds,
    uint64_t sequence
) {
    RT64::PerformanceProfiler::recordRuntimeGraphicsActionDequeued(
        type == ultramodern::events::GraphicsActionType::ScreenUpdate,
        queued_nanoseconds, dequeued_nanoseconds, sequence);
}

void profile_graphics_action_completed(
    ultramodern::events::GraphicsActionType type,
    uint64_t dequeued_nanoseconds,
    uint64_t completed_nanoseconds,
    uint64_t sequence
) {
    RT64::PerformanceProfiler::recordRuntimeGraphicsActionCompleted(
        type == ultramodern::events::GraphicsActionType::ScreenUpdate,
        dequeued_nanoseconds, completed_nanoseconds, sequence);
}

bool acquire_single_instance() {
#if defined(_WIN32)
    g_single_instance_mutex = CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\BumbleRecomp.SingleInstance"
    );
    return g_single_instance_mutex != nullptr &&
        GetLastError() != ERROR_ALREADY_EXISTS;
#else
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    std::filesystem::path lock_path = runtime_dir != nullptr && *runtime_dir != '\0'
        ? std::filesystem::path(runtime_dir)
        : std::filesystem::temp_directory_path();
    lock_path /= "bumble-recomp-" + std::to_string(getuid()) + ".lock";
    g_single_instance_fd = open(
        lock_path.c_str(),
        O_CREAT | O_CLOEXEC | O_RDWR,
        0600
    );
    return g_single_instance_fd >= 0 &&
        flock(g_single_instance_fd, LOCK_EX | LOCK_NB) == 0;
#endif
}

void log_stage(const char* stage) {
    std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=%s\n", stage);
    std::fflush(stderr);
}

void log_error(const char* stage, unsigned long value) {
    std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=%s error=%lu\n", stage, value);
    std::fflush(stderr);
}

std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    std::array<char, PATH_MAX> buffer{};
    const ssize_t length = readlink(
        "/proc/self/exe",
        buffer.data(),
        buffer.size() - 1u
    );
    if (length <= 0) {
        return {};
    }
    buffer[static_cast<size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data()).parent_path();
#endif
}

std::filesystem::path default_release_data_root() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer) / "BumbleRecomp";
    }
    return executable_directory() / "userdata";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdg) / "BumbleRecomp";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" /
            "BumbleRecomp";
    }
    return executable_directory() / "userdata";
#endif
}

void configure_rt64_pipeline_cache(const std::filesystem::path& data_root) {
    const std::filesystem::path vulkan_cache_path =
        data_root / "cache" / "rt64-vulkan-pipelines.bin";
#if defined(_WIN32)
    const std::filesystem::path d3d12_cache_path =
        data_root / "cache" / "rt64-d3d12-pipelines.bin";
    const errno_t vulkan_result = _wputenv_s(
        L"RT64_VULKAN_PIPELINE_CACHE",
        vulkan_cache_path.c_str()
    );
    const errno_t d3d12_result = _wputenv_s(
        L"RT64_D3D12_PIPELINE_CACHE",
        d3d12_cache_path.c_str()
    );
    const int result = vulkan_result != 0
        ? static_cast<int>(vulkan_result)
        : static_cast<int>(d3d12_result);
#else
    const int result =
        setenv(
            "RT64_VULKAN_PIPELINE_CACHE",
            vulkan_cache_path.c_str(),
            1
        );
#endif
    if (result != 0) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=pipeline_cache_path_failed error=%d\n",
            static_cast<int>(result)
        );
    }
}

#if defined(_WIN32)
void update_cursor_clip() {
    if (g_validation_synthetic_input) {
        ClipCursor(nullptr);
        return;
    }
    if (!bumble::modern_controls::mouse_captured() ||
        g_window == nullptr || !IsWindow(g_window)) {
        ClipCursor(nullptr);
        return;
    }

    RECT client{};
    if (!GetClientRect(g_window, &client)) {
        log_error("modern_mouse_client_rect_failed", GetLastError());
        return;
    }
    if (IsIconic(g_window) || client.right <= client.left ||
        client.bottom <= client.top) {
        ClipCursor(nullptr);
        return;
    }
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (!ClientToScreen(g_window, &top_left) ||
        !ClientToScreen(g_window, &bottom_right)) {
        log_error("modern_mouse_client_to_screen_failed", GetLastError());
        return;
    }
    // Client bounds are exclusive; cursor clipping includes the boundary pixel.
    const RECT screen_rect{
        top_left.x,
        top_left.y,
        bottom_right.x - 1,
        bottom_right.y - 1,
    };
    RECT current_clip{};
    const bool clip_matches = GetClipCursor(&current_clip) &&
        current_clip.left == screen_rect.left &&
        current_clip.top == screen_rect.top &&
        current_clip.right == screen_rect.right &&
        current_clip.bottom == screen_rect.bottom;
    if (clip_matches) {
        return;
    }
    if (!ClipCursor(&screen_rect)) {
        log_error("modern_mouse_clip_failed", GetLastError());
        return;
    }
    ++g_mouse_clip_reassert_count;
    if (g_mouse_clip_reassert_count == 1 ||
        (g_mouse_clip_reassert_count % 120u) == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_mouse_clip_reasserted"
            " count=%llu rect=(%ld,%ld,%ld,%ld)\n",
            static_cast<unsigned long long>(g_mouse_clip_reassert_count),
            screen_rect.left,
            screen_rect.top,
            screen_rect.right,
            screen_rect.bottom
        );
        std::fflush(stderr);
    }
}

void refresh_mouse_capture() {
    const bool active = g_modern_controls_requested && g_window_focused &&
        bumble::modern_controls::gameplay_input_active() &&
        !bumble::level_editor::menu_open() &&
        (g_validation_synthetic_input ||
         bumble::native_io::keyboard_mouse_gameplay_active());
    bumble::modern_controls::set_mouse_capture(active);
    update_cursor_clip();
    if (g_window != nullptr && IsWindow(g_window)) {
        SetCursor(
            active ? nullptr :
                LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))
        );
    }
}

bool game_window_has_input_focus() {
    if (g_window == nullptr || !IsWindow(g_window)) {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    return foreground == g_window ||
        (foreground != nullptr &&
         GetAncestor(foreground, GA_ROOT) == g_window);
}

void reconcile_window_focus(const char* source) {
    const bool physical_focused = game_window_has_input_focus();
    const bool focused = g_validation_synthetic_input || physical_focused;
    if (focused == g_window_focused &&
        focused == bumble::modern_controls::window_focused()) {
        return;
    }
    bumble::native_io::clear_keyboard_key_state();
    if (!focused) {
        bumble::modern_controls::set_primary_fire(false);
    }
    g_window_focused = focused;
    bumble::modern_controls::set_window_focused(focused);
    refresh_mouse_capture();
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=window_focus_reconciled"
        " active=%d source=%s foreground=%d keyboard_focus=%d\n",
        focused ? 1 : 0,
        source,
        physical_focused ? 1 : 0,
        GetFocus() == g_window ? 1 : 0
    );
    std::fflush(stderr);
}

bool register_raw_mouse(HWND window) {
    RAWINPUTDEVICE mouse{
        .usUsagePage = 0x01,
        .usUsage = 0x02,
        .dwFlags = RIDEV_INPUTSINK,
        .hwndTarget = window,
    };
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
        log_error("modern_mouse_registration_failed", GetLastError());
        return false;
    }
    log_stage("modern_mouse_registered");
    return true;
}

void handle_raw_mouse(LPARAM lparam) {
    if (!bumble::modern_controls::mouse_captured()) {
        return;
    }
    UINT size = 0;
    if (GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lparam),
            RID_INPUT,
            nullptr,
            &size,
            sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return;
    }
    thread_local std::vector<std::byte> storage;
    storage.resize(size);
    if (GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lparam),
            RID_INPUT,
            storage.data(),
            &size,
            sizeof(RAWINPUTHEADER)) != size) {
        return;
    }
    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(storage.data());
    if (input->header.dwType != RIM_TYPEMOUSE ||
        (input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        return;
    }
    bumble::modern_controls::add_raw_mouse_delta(
        input->data.mouse.lLastX,
        input->data.mouse.lLastY
    );
}

void handle_validation_synthetic_mouse(LPARAM lparam) {
    if (!g_validation_synthetic_input) {
        return;
    }
    const uint32_t packed = static_cast<uint32_t>(lparam);
    const int32_t delta_x = static_cast<int16_t>(packed & 0xFFFFu);
    const int32_t delta_y = static_cast<int16_t>((packed >> 16u) & 0xFFFFu);
    if (bumble::level_editor::menu_open() ||
        bumble::graphics_options::mouse_menu_navigation_active()) {
        RECT client{};
        if (!GetClientRect(g_window, &client) ||
            client.right <= 0 || client.bottom <= 0) {
            return;
        }
        const auto pointer = bumble::graphics_options::pointer_snapshot();
        const int32_t x = static_cast<int32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(pointer.client_x) + delta_x,
            0, client.right - 1));
        const int32_t y = static_cast<int32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(pointer.client_y) + delta_y,
            0, client.bottom - 1));
        bumble::graphics_options::update_menu_pointer(
            x, y, client.right, client.bottom, false, true);
        return;
    }
    if (!bumble::modern_controls::mouse_captured()) {
        return;
    }
    bumble::modern_controls::add_raw_mouse_delta(delta_x, delta_y);
}

void log_abort_stack(int) {
    void* frames[48]{};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    std::fprintf(stderr, "BUMBLE_CRASH stage=abort thread=%lu frames=%u\n",
        GetCurrentThreadId(), static_cast<unsigned>(count));
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[i]), &module)) {
            GetModuleFileNameW(module, path, MAX_PATH);
        }
        std::fprintf(stderr, "BUMBLE_CRASH frame=%u module=%ls rva=0x%" PRIXPTR "\n",
            static_cast<unsigned>(i), module ? path : L"<unknown>",
            reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module));
    }
    std::fflush(stderr);
    // Return to CRT abort: preserve its fatal-exit and WER behavior.
}

void log_terminate() noexcept {
    try {
        if (const auto exception = std::current_exception()) {
            std::rethrow_exception(exception);
        }
        std::fprintf(stderr, "BUMBLE_CRASH stage=terminate exception=none\n");
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "BUMBLE_CRASH stage=terminate what=%s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "BUMBLE_CRASH stage=terminate exception=non_std\n");
    }
    std::fflush(stderr);
    std::abort();
}

LONG WINAPI log_unhandled_exception(EXCEPTION_POINTERS* exception) {
    const EXCEPTION_RECORD* record = exception->ExceptionRecord;
    HMODULE module = nullptr;
    wchar_t module_path[MAX_PATH]{};
    const auto address = record->ExceptionAddress;
    const bool found_module = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),
        &module
    ) != 0;
    if (found_module) {
        GetModuleFileNameW(module, module_path, MAX_PATH);
    }
    const auto module_offset = found_module
        ? reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)
        : 0;

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=unhandled_exception code=0x%08lX "
        "address=%p thread=%lu module=%ls module_offset=0x%" PRIXPTR "\n",
        record->ExceptionCode,
        address,
        GetCurrentThreadId(),
        found_module ? module_path : L"<unknown>",
        module_offset
    );
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

#else
void update_cursor_clip() {
}

void refresh_mouse_capture() {
    const bool active = g_modern_controls_requested && g_window_focused &&
        bumble::modern_controls::gameplay_input_active() &&
        !bumble::level_editor::menu_open() &&
        (g_validation_synthetic_input ||
         bumble::native_io::keyboard_mouse_gameplay_active());
    bumble::modern_controls::set_mouse_capture(active);
    SDL_SetRelativeMouseMode(active ? SDL_TRUE : SDL_FALSE);
    SDL_ShowCursor(active ? SDL_DISABLE : SDL_ENABLE);
}

void reconcile_window_focus(const char* source) {
    const bool physical_focused = g_window != nullptr &&
        (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool focused = g_validation_synthetic_input || physical_focused;
    if (focused == g_window_focused &&
        focused == bumble::modern_controls::window_focused()) {
        return;
    }
    bumble::native_io::clear_keyboard_key_state();
    if (!focused) {
        bumble::modern_controls::set_primary_fire(false);
    }
    g_window_focused = focused;
    bumble::modern_controls::set_window_focused(focused);
    refresh_mouse_capture();
    std::fprintf(
        stderr,
        "BUMBLE stage=window_focus active=%d source=%s\n",
        focused ? 1 : 0,
        source
    );
}
#endif

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task != nullptr && task->t.type == M_AUDTASK) {
        const uint32_t count = g_audio_tasks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count == 60 || count == 600) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_rsp_task count=%" PRIu32 "\n",
                count
            );
            std::fflush(stderr);
        }
        return bumble::native_rsp_task::select_audio_microcode(task);
    }

    const uint32_t task_type = task == nullptr ? UINT32_MAX : task->t.type;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=unsupported_rsp_task type=%" PRIu32 "\n",
        task_type
    );
    std::fflush(stderr);
    return nullptr;
}

void on_game_init(uint8_t* rdram, recomp_context*) {
    log_stage("game_init_callback_enter");
    install_buck_cart_rom_mirror(rdram);
    log_stage("cart_rom_mirror_installed offset=0x30000000 size=0x00C00000 read_only=1");
    unload_overlays(kLoadedCoreAlias, kLoadedCoreSize);
    log_stage("loaded_core_alias_unloaded");
    load_overlays(kLoadedCoreRom, kLoadedCoreRam, kLoadedCoreSize);
    log_stage("loaded_core_mapping_registered");
    bumble::native_io::arm_replay();
}

void diagnostic_entrypoint(uint8_t* rdram, recomp_context* context) {
    log_stage("guest_entrypoint_enter");
    context->r29 = kInitialStack;
    for (uint32_t address = kInitialBssStart; address < kInitialBssEnd; address += 4) {
        MEM_W(0, static_cast<int32_t>(address)) = 0;
    }
    log_stage("guest_initial_bss_cleared");
    buck_main(rdram, context);
    log_stage("entrypoint_thread_terminated_after_first_guest_thread_start");
    throw ultramodern::thread_terminated{};
}

#if defined(_WIN32)
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT graphics_options_result = 0;
    const bool physical_input_suppressed =
        g_validation_synthetic_input ||
        bumble::native_io::replay_configured();
    if (physical_input_suppressed &&
        (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_MOUSEWHEEL ||
         message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
         message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
         message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
         message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)) {
        return 0;
    }
    if (bumble::graphics_options::handle_game_window_message(
            window,
            message,
            wparam,
            lparam,
            graphics_options_result)) {
        return graphics_options_result;
    }
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (physical_input_suppressed) {
            return 0;
        }
        bumble::native_io::set_keyboard_key_state(
            static_cast<uint32_t>(wparam),
            true
        );
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (physical_input_suppressed) {
            return 0;
        }
        bumble::native_io::set_keyboard_key_state(
            static_cast<uint32_t>(wparam),
            false
        );
        break;
    case WM_INPUT:
        if (!physical_input_suppressed) {
            handle_raw_mouse(lparam);
        }
        break;
    case kValidationSyntheticMouseMessage:
        handle_validation_synthetic_mouse(lparam);
        break;
    case kValidationSyntheticKeyMessage:
        if (g_validation_synthetic_input && wparam <= 0xFFu) {
            bumble::native_io::set_keyboard_key_state(
                static_cast<uint32_t>(wparam),
                lparam != 0
            );
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (!physical_input_suppressed) {
            bumble::native_io::add_mouse_wheel_delta(GET_WHEEL_DELTA_WPARAM(wparam));
        }
        return 0;
    case kValidationSyntheticWheelMessage:
        if (g_validation_synthetic_input && lparam >= -7680 && lparam <= 7680) {
            bumble::native_io::add_mouse_wheel_delta(static_cast<int>(lparam));
        }
        return 0;
    case kValidationSyntheticMouseButtonMessage:
        if (g_validation_synthetic_input &&
            wparam >= static_cast<WPARAM>(
                bumble::input_bindings::MouseButton::Left
            ) &&
            wparam <= static_cast<WPARAM>(
                bumble::input_bindings::MouseButton::X2
            )) {
            if (wparam == static_cast<WPARAM>(
                    bumble::input_bindings::MouseButton::Left)) {
                if (lparam == 0) {
                    bumble::graphics_options::release_menu_pointer_click();
                } else if (bumble::level_editor::menu_open() ||
                    bumble::graphics_options::mouse_menu_navigation_active()) {
                    const auto pointer =
                        bumble::graphics_options::pointer_snapshot();
                    bumble::graphics_options::update_menu_pointer(
                        pointer.client_x, pointer.client_y,
                        pointer.client_width, pointer.client_height,
                        true, pointer.inside_client);
                }
            }
            bumble::native_io::set_mouse_button_state(
                static_cast<bumble::input_bindings::MouseButton>(wparam),
                lparam != 0
            );
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (g_modern_controls_requested ||
            bumble::level_editor::active() ||
            bumble::graphics_options::mouse_menu_navigation_active() ||
            bumble::input_bindings::capture_state().active) {
            if (physical_input_suppressed) {
                return 0;
            }
            bumble::native_io::set_mouse_button_state(
                bumble::input_bindings::MouseButton::Left,
                true
            );
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_modern_controls_requested ||
            bumble::level_editor::active() ||
            bumble::graphics_options::mouse_menu_navigation_active() ||
            bumble::input_bindings::capture_state().active) {
            if (physical_input_suppressed) {
                return 0;
            }
            bumble::native_io::set_mouse_button_state(
                bumble::input_bindings::MouseButton::Left,
                false
            );
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (g_modern_controls_requested ||
            bumble::graphics_options::mouse_menu_navigation_active() ||
            bumble::input_bindings::capture_state().active) {
            if (physical_input_suppressed) {
                return 0;
            }
            const bool pressed =
                message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN ||
                message == WM_XBUTTONDOWN;
            const auto button =
                message == WM_RBUTTONDOWN || message == WM_RBUTTONUP
                ? bumble::input_bindings::MouseButton::Right
                : message == WM_MBUTTONDOWN || message == WM_MBUTTONUP
                    ? bumble::input_bindings::MouseButton::Middle
                    : (GET_XBUTTON_WPARAM(wparam) == XBUTTON1
                        ? bumble::input_bindings::MouseButton::X1
                        : bumble::input_bindings::MouseButton::X2);
            bumble::native_io::set_mouse_button_state(button, pressed);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        bumble::native_io::clear_keyboard_key_state();
        g_window_focused = true;
        bumble::modern_controls::set_window_focused(true);
        refresh_mouse_capture();
        return 0;
    case WM_KILLFOCUS:
        if (g_validation_synthetic_input) {
            return 0;
        }
        bumble::native_io::clear_keyboard_key_state();
        bumble::modern_controls::set_primary_fire(false);
        g_window_focused = false;
        bumble::modern_controls::set_window_focused(false);
        refresh_mouse_capture();
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        update_cursor_clip();
        break;
    case WM_SETCURSOR:
        if (bumble::modern_controls::mouse_captured() &&
            LOWORD(lparam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        bumble::native_io::clear_keyboard_key_state();
        bumble::modern_controls::set_primary_fire(false);
        g_window_focused = false;
        bumble::modern_controls::set_window_focused(false);
        refresh_mouse_capture();
        // Defer close until RT64 is initialized; early teardown can strand its threads.
        g_window_close_requested.store(true, std::memory_order_release);
        return 0;
    case WM_DESTROY:
        bumble::native_io::clear_keyboard_key_state();
        bumble::modern_controls::set_primary_fire(false);
        g_window_focused = false;
        bumble::modern_controls::set_window_focused(false);
        bumble::modern_controls::set_mouse_capture(false);
        ClipCursor(nullptr);
        g_window = nullptr;
        log_stage("window_destroyed");
        std::fprintf(stderr, "BUMBLE_RT64_SHUTDOWN stage=runtime_quit_begin\n");
        std::fflush(stderr);
        ultramodern::quit();
        std::fprintf(stderr, "BUMBLE_RT64_SHUTDOWN stage=runtime_quit_returned\n");
        std::fflush(stderr);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool create_visible_window() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = window_proc,
        .cbClsExtra = 0,
        .cbWndExtra = 0,
        .hInstance = instance,
        .hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        .hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)),
        .lpszMenuName = nullptr,
        .lpszClassName = kWindowClassName,
        .hIconSm = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
    };

    if (RegisterClassExW(&window_class) == 0) {
        log_error("window_class_registration_failed", GetLastError());
        return false;
    }

    RECT window_rect{
        0,
        0,
        static_cast<LONG>(bumble::graphics_options::initial_client_width()),
        static_cast<LONG>(bumble::graphics_options::initial_client_height()),
    };
    AdjustWindowRectEx(&window_rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    g_window = CreateWindowExW(
        0,
        kWindowClassName,
        L"Bumble",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr
    );
    if (g_window == nullptr) {
        log_error("visible_window_creation_failed", GetLastError());
        UnregisterClassW(kWindowClassName, instance);
        return false;
    }
    bumble::graphics_options::attach_game_window(g_window);

    if (g_modern_controls_requested && !g_validation_synthetic_input &&
        !register_raw_mouse(g_window)) {
        DestroyWindow(g_window);
        g_window = nullptr;
        UnregisterClassW(kWindowClassName, instance);
        return false;
    }

    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);
    if (g_modern_controls_requested &&
        !bumble::native_io::replay_configured() &&
        !g_validation_synthetic_input) {
        BringWindowToTop(g_window);
        const BOOL foreground_requested = SetForegroundWindow(g_window);
        SetActiveWindow(g_window);
        SetFocus(g_window);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=startup_input_focus_requested"
            " foreground_request=%d foreground=%d keyboard_focus=%d"
            " replay=0\n",
            foreground_requested ? 1 : 0,
            GetForegroundWindow() == g_window ? 1 : 0,
            GetFocus() == g_window ? 1 : 0
        );
        std::fflush(stderr);
    }
    reconcile_window_focus("window_created");
    log_stage("visible_window_created");
    return true;
}

void pump_messages(void*) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    reconcile_window_focus("message_pump");
    bumble::native_io::pump_events();
    const bool capture_target = g_modern_controls_requested &&
        g_window_focused &&
        bumble::modern_controls::gameplay_input_active() &&
        !bumble::level_editor::menu_open() &&
        (g_validation_synthetic_input ||
         bumble::native_io::keyboard_mouse_gameplay_active());
    if (bumble::modern_controls::mouse_captured() != capture_target) {
        refresh_mouse_capture();
    }
    if (bumble::modern_controls::mouse_captured()) {
        update_cursor_clip();
    }

    if (g_window_close_requested.load(std::memory_order_acquire)) {
        const bool guest_started =
            g_game_start_requested.load(std::memory_order_acquire);
        const bool guest_shutdown_ready =
            !guest_started ||
            g_replay_close_requested.load(std::memory_order_acquire) ||
            bumble::graphics_options::runtime_shutdown_ready();
        if (guest_shutdown_ready &&
            bumble::rt64_renderer::request_shutdown()) {
            log_stage(
                guest_started
                    ? "window_close_committed_guest_stable"
                    : "window_close_committed_before_guest_start"
            );
            ultramodern::quit();
            PostQuitMessage(0);
            return;
        }
        if (!g_window_close_deferred_logged.exchange(
                true,
                std::memory_order_acq_rel
            )) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_SHUTDOWN stage=window_close_deferred"
                " guest_started=%d guest_shutdown_ready=%d\n",
                guest_started ? 1 : 0,
                guest_shutdown_ready ? 1 : 0
            );
            std::fflush(stderr);
        }
        return;
    }

    if (bumble::native_io::replay_complete() &&
        bumble::native_checkpoint::widescreen_hud_validation_complete() &&
        !g_replay_close_requested.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_close_requested tick=%llu semantic_success=%d\n",
            static_cast<unsigned long long>(bumble::native_io::replay_tick()),
            bumble::native_io::replay_semantic_success() ? 1 : 0
        );
        std::fflush(stderr);
        if (g_window != nullptr && IsWindow(g_window)) {
            PostMessageW(g_window, WM_CLOSE, 0, 0);
        }
    }

    if (bumble::rt64_renderer::screen_update_count() >= 2 &&
        !g_game_start_requested.exchange(true, std::memory_order_acq_rel)) {
        log_stage("game_start_requested_after_visible_vi");
        recomp::start_game(kGameId);
    }
}

void destroy_visible_window() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    bumble::native_io::clear_keyboard_key_state();
    bumble::modern_controls::set_primary_fire(false);
    g_window_focused = false;
    bumble::modern_controls::set_window_focused(false);
    refresh_mouse_capture();
    if (g_window != nullptr && IsWindow(g_window)) {
        DestroyWindow(g_window);
    }
    UnregisterClassW(kWindowClassName, instance);
}
#else
bool create_visible_window() {
    g_window = SDL_CreateWindow(
        "Bumble",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        static_cast<int>(bumble::graphics_options::initial_client_width()),
        static_cast<int>(bumble::graphics_options::initial_client_height()),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN
    );
    if (g_window == nullptr) {
        std::fprintf(stderr, "BUMBLE stage=window_create_failed error=%s\n", SDL_GetError());
        return false;
    }
    bumble::graphics_options::attach_game_window(g_window);
    g_window_focused = true;
    bumble::modern_controls::set_window_focused(true);
    refresh_mouse_capture();
    log_stage("visible_window_created");
    return true;
}

void pump_messages(void*) {
    bumble::native_io::pump_events();
    reconcile_window_focus("sdl_event_pump");

    const bool capture_target = g_modern_controls_requested &&
        g_window_focused &&
        bumble::modern_controls::gameplay_input_active() &&
        !bumble::level_editor::menu_open() &&
        (g_validation_synthetic_input ||
         bumble::native_io::keyboard_mouse_gameplay_active());
    if (bumble::modern_controls::mouse_captured() != capture_target) {
        refresh_mouse_capture();
    }

    const bool close_requested = bumble::native_io::quit_requested() ||
        (bumble::native_io::replay_complete() &&
         bumble::native_checkpoint::widescreen_hud_validation_complete());
    if (close_requested &&
        !g_replay_close_requested.exchange(true, std::memory_order_acq_rel)) {
        g_window_focused = false;
        bumble::modern_controls::set_window_focused(false);
        refresh_mouse_capture();
        bumble::rt64_renderer::request_shutdown();
        ultramodern::quit();
    }

    if (bumble::rt64_renderer::screen_update_count() >= 2 &&
        !g_game_start_requested.exchange(true, std::memory_order_acq_rel)) {
        log_stage("game_start_requested_after_visible_vi");
        recomp::start_game(kGameId);
    }
}

void destroy_visible_window() {
    bumble::native_io::clear_keyboard_key_state();
    bumble::modern_controls::set_primary_fire(false);
    g_window_focused = false;
    bumble::modern_controls::set_window_focused(false);
    bumble::modern_controls::set_mouse_capture(false);
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    log_stage("window_destroyed");
}
#endif

const char* validation_error_name(recomp::RomValidationError error) {
    switch (error) {
    case recomp::RomValidationError::Good:
        return "good";
    case recomp::RomValidationError::FailedToOpen:
        return "failed_to_open";
    case recomp::RomValidationError::NotARom:
        return "not_a_rom";
    case recomp::RomValidationError::IncorrectRom:
        return "incorrect_rom";
    case recomp::RomValidationError::NotYet:
        return "not_yet";
    case recomp::RomValidationError::IncorrectVersion:
        return "incorrect_version";
    case recomp::RomValidationError::OtherError:
        return "other_error";
    }
    return "unknown";
}

bool parse_connected_pak(
    const std::string& value,
    ultramodern::input::Pak& connected_pak
) {
    using Pak = ultramodern::input::Pak;
    if (value == "None") {
        connected_pak = Pak::None;
        return true;
    }
    if (value == "RumblePak") {
        connected_pak = Pak::RumblePak;
        return true;
    }
    if (value == "ControllerPak") {
        connected_pak = Pak::ControllerPak;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    bool release_diagnostic_logging = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--diagnostic-logging") {
            release_diagnostic_logging = true;
            break;
        }
    }
    if (!release_diagnostic_logging) {
#if defined(_WIN32)
        FILE* null_stream = nullptr;
        (void)freopen_s(&null_stream, "NUL", "w", stdout);
        (void)freopen_s(&null_stream, "NUL", "w", stderr);
#else
        (void)std::freopen("/dev/null", "w", stdout);
        (void)std::freopen("/dev/null", "w", stderr);
#endif
    } else {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    bumble::object_cull_telemetry::set_diagnostics_enabled(
        release_diagnostic_logging
    );
    bumble::electric_effect::set_diagnostics_enabled(
        release_diagnostic_logging
    );
    bumble::weapon_system::set_diagnostics_enabled(
        release_diagnostic_logging
    );
#if defined(_WIN32)
    SetUnhandledExceptionFilter(log_unhandled_exception);
    if (release_diagnostic_logging) {
        std::signal(SIGABRT, log_abort_stack);
        std::set_terminate(log_terminate);
    }
#endif
    if (!acquire_single_instance()) {
        log_stage("duplicate_instance_rejected");
        return 0;
    }

    const char* validation_synthetic_input =
        std::getenv("BUMBLE_RT64_VALIDATION_SYNTHETIC_INPUT");
    g_validation_synthetic_input = validation_synthetic_input != nullptr &&
        std::string(validation_synthetic_input) == "1";
    if (g_validation_synthetic_input) {
        log_stage(
            "validation_synthetic_input_enabled physical_keyboard_suppressed=1"
            " physical_raw_mouse_suppressed=1"
            " physical_mouse_buttons_suppressed=1 focus_spoofed=1"
        );
    }

    std::filesystem::path replay_path;
    std::filesystem::path controller_pak_root;
    std::filesystem::path data_root;
    std::string replay_mask = "NONE";
    const bool direct_launch = argc == 1;
    bool modern_controls = true;
    bool control_mode_explicit = false;
    bool show_intro = false;
    bool invert_mouse_y = false;
    bool mouse_sensitivity_explicit = false;
    bool first_run_only = false;
    float mouse_sensitivity = 0.15f;
    ultramodern::input::Pak connected_pak = ultramodern::input::Pak::ControllerPak;
    bool connected_pak_explicit = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--replay" && index + 1 < argc) {
            replay_path = std::filesystem::absolute(argv[++index]);
        } else if (argument == "--replay-mask" && index + 1 < argc) {
            replay_mask = argv[++index];
        } else if (argument == "--controller-pak-root" && index + 1 < argc) {
            controller_pak_root = std::filesystem::absolute(argv[++index]);
        } else if (argument == "--data-root" && index + 1 < argc) {
            data_root = std::filesystem::absolute(argv[++index]);
        } else if (argument == "--connected-pak" && index + 1 < argc) {
            connected_pak_explicit = true;
            if (!parse_connected_pak(argv[++index], connected_pak)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=argument_error value=%s\n",
                    argv[index]
                );
                return 2;
            }
        } else if (argument == "--modern-controls") {
            modern_controls = true;
            control_mode_explicit = true;
        } else if (argument == "--classic-controls") {
            modern_controls = false;
            control_mode_explicit = true;
        } else if (argument == "--show-intro") {
            show_intro = true;
        } else if (argument == "--mouse-sensitivity" && index + 1 < argc) {
            try {
                const std::string value = argv[++index];
                size_t consumed = 0;
                mouse_sensitivity = std::stof(value, &consumed);
                if (consumed != value.size()) {
                    throw std::invalid_argument("trailing mouse sensitivity text");
                }
                mouse_sensitivity_explicit = true;
            } catch (...) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=argument_error value=%s\n",
                    argv[index]
                );
                return 2;
            }
            if (!std::isfinite(mouse_sensitivity) ||
                mouse_sensitivity < 0.01f || mouse_sensitivity > 2.0f) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=argument_error value=%s\n",
                    argv[index]
                );
                return 2;
            }
        } else if (argument == "--invert-mouse-y") {
            invert_mouse_y = true;
        } else if (argument == "--first-run-only") {
            first_run_only = true;
        } else if (argument == "--diagnostic-logging") {
            // Handled before startup.
        } else {
            std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=argument_error value=%s\n", argument.c_str());
            return 2;
        }
    }
    if (!replay_path.empty() && !control_mode_explicit) {
        modern_controls = false;
    }
    if ((replay_path.empty() && replay_mask != "NONE") ||
        (!replay_path.empty() && connected_pak_explicit) ||
        (!replay_path.empty() && control_mode_explicit) ||
        (!modern_controls && (mouse_sensitivity_explicit || invert_mouse_y)) ||
        (first_run_only && !replay_path.empty())) {
        std::fprintf(
            stderr,
            "Usage: %s [ROM] [options]\nInvalid option combination.\n",
            argv[0]
        );
        return 2;
    }

    std::filesystem::path rom_path;
    if (data_root.empty()) {
        data_root = default_release_data_root();
    }
#if defined(_WIN32)
    if (!bumble::portable::prepare(data_root, release_diagnostic_logging)) {
        return 3;
    }
#endif
    configure_rt64_pipeline_cache(data_root);
    if (direct_launch) {
        const auto selected_rom = bumble::first_run::resolve_rom(data_root);
        if (!selected_rom.has_value()) {
            return 2;
        }
        rom_path = *selected_rom;
    } else {
        rom_path = std::filesystem::absolute(argv[1]);
    }
    const std::filesystem::path config_path = data_root / "config";
    if (controller_pak_root.empty()) {
        controller_pak_root = data_root / "controller-pak";
    }
    std::error_code directory_error;
    std::filesystem::create_directories(config_path, directory_error);
    if (directory_error) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=config_directory_failed error=%s\n",
            directory_error.message().c_str()
        );
        return 3;
    }
    directory_error.clear();
    std::filesystem::create_directories(controller_pak_root, directory_error);
    if (directory_error) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=controller_pak_directory_failed path=%s error=%s\n",
            controller_pak_root.string().c_str(),
            directory_error.message().c_str()
        );
        return 3;
    }

    if (!bumble::graphics_options::initialize(config_path)) {
        return 3;
    }
    if (g_validation_synthetic_input) {
        bumble::input_bindings::set_device_mode(
            bumble::input_bindings::InputDeviceMode::KeyboardMouse
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=validation_input_profile"
            " requested=keyboard_mouse persisted_user_setting=unchanged\n"
        );
        std::fflush(stderr);
    }

    recomp::register_config_path(config_path);
    bumble::controller_pak::configure_storage_root(controller_pak_root);
    recomp::GameEntry game{
        .rom_hash = kRomHash,
        .internal_name = "BUCK BUMBLE",
        .game_id = kGameId,
        .mod_game_id = "",
        .save_type = recomp::SaveType::None,
        .is_enabled = true,
        .decompression_routine = nullptr,
        .has_compressed_code = false,
        .entrypoint_address = kEntrypointAddress,
        .entrypoint = diagnostic_entrypoint,
        .thread_create_callback = nullptr,
        .on_init_callback = on_game_init,
    };
    if (!recomp::register_game(game)) {
        log_stage("game_registration_failed");
        return 4;
    }
    log_stage("game_registered");

    register_bumble_overlays();
    log_stage("overlays_registered");

    const recomp::RomValidationError validation = recomp::select_rom(rom_path, game.game_id);
    if (validation != recomp::RomValidationError::Good) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rom_selection_failed result=%s value=%d\n",
            validation_error_name(validation),
            static_cast<int>(validation)
        );
        if (direct_launch) {
#if defined(_WIN32)
            const std::wstring message =
                L"Select a Buck Bumble (USA) revision-0 ROM.";
            MessageBoxW(
                nullptr,
                message.c_str(),
                L"Bumble - incompatible ROM",
                MB_OK | MB_ICONERROR
            );
#endif
        }
        return 5;
    }
    log_stage("rom_selected_and_copied_to_ignored_runtime_store");
    if (!recomp::load_stored_rom(game.game_id)) {
        log_stage("stored_rom_preflight_failed");
        return 6;
    }
    log_stage("stored_rom_preflight_passed");
    const auto assets = bumble::first_run::ensure_assets(data_root, rom_path, !replay_path.empty());
    if (assets == bumble::first_run::AssetResult::Cancelled) return 0;
    if (assets != bumble::first_run::AssetResult::Ready) {
        log_stage("first_run_asset_generation_failed");
        return 8;
    }

    if (replay_path.empty()) {
        if (!bumble::first_run::remember_rom(data_root, rom_path)) {
            return 8;
        }
        if (first_run_only) {
            log_stage("first_run_ready");
            return 0;
        }
    }

    if (!replay_path.empty() && !bumble::input_bindings::validate_default_contracts()) {
        return 2;
    }
    bumble::rt64_renderer::set_contract_validation_enabled(!replay_path.empty());
    if (!replay_path.empty() &&
        !bumble::native_io::configure_replay(
            replay_path.string().c_str(), replay_mask.c_str())) {
        return 7;
    }
    bumble::native_checkpoint::configure_widescreen_hud_validation(
        !replay_path.empty() &&
        std::getenv("BUMBLE_WIDESCREEN_HUD_CAPTURE_DIR") != nullptr
    );
    if (bumble::native_io::replay_uses_modern_controls()) {
        modern_controls = true;
    }
    if (replay_path.empty() &&
        !bumble::native_io::configure_connected_pak(0, connected_pak)) {
        return 7;
    }

    g_modern_controls_requested = modern_controls;
    bumble::startup_flow::configure(
        !show_intro,
        true
    );
    bumble::startup_flow::configure_rumble_prompt_bypass(true);
    bumble::modern_controls::configure(
        modern_controls,
        mouse_sensitivity,
        invert_mouse_y
    );
    if (bumble::native_io::replay_uses_modern_controls()) {
        bumble::modern_controls::set_replay_automation(true);
    }
    bumble::modern_sky::set_enabled(true);
    const std::filesystem::path asset_root = data_root / "assets";
    const std::filesystem::path menu_path = asset_root / "menu";
    if (!bumble::menu_background::configure(menu_path) ||
        !bumble::text_overlay::configure_menu_background(menu_path) ||
        !bumble::death_screen::configure(asset_root / "death") ||
        !bumble::game_completion_screen::configure(
            asset_root / "completion"
        )) {
        return 8;
    }

    if (!bumble::native_io::initialize()) {
        return 9;
    }
    if (!bumble::level_editor::initialize(
            data_root,
            rom_path,
            replay_path.empty()
        )) {
        bumble::native_io::shutdown();
        return 9;
    }
    bumble::native_io::poll_input();

    if (!create_visible_window()) {
        bumble::level_editor::shutdown();
        bumble::native_io::shutdown();
        return 10;
    }

    ultramodern::renderer::GraphicsConfig graphics_config{};
    graphics_config.developer_mode = false;
    graphics_config.res_option = ultramodern::renderer::Resolution::Auto;
    graphics_config.wm_option = ultramodern::renderer::WindowMode::Windowed;
    graphics_config.hr_option = ultramodern::renderer::HUDRatioMode::Full;
    graphics_config.api_option = ultramodern::renderer::GraphicsApi::Auto;
    graphics_config.ar_option = ultramodern::renderer::AspectRatio::Expand;
    graphics_config.msaa_option = ultramodern::renderer::Antialiasing::MSAA2X;
    graphics_config.rr_option = ultramodern::renderer::RefreshRate::Display;
    graphics_config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
    graphics_config.rr_manual_value = 60;
    graphics_config.ds_option = 1;
    bumble::graphics_options::apply_to_graphics_config(graphics_config);
    bumble::rt64_renderer::set_fog_scale(
        bumble::graphics_options::fog_scale()
    );
    ultramodern::renderer::set_graphics_config(graphics_config);

    recomp::Configuration config{};
    config.project_version = recomp::Version{
        .major = 0,
        .minor = 0,
        .patch = 0,
        .suffix = "-unified",
    };
#if defined(_WIN32)
    config.window_handle = ultramodern::renderer::WindowHandle{
        .window = g_window,
        .thread_id = GetCurrentThreadId(),
    };
#else
    config.window_handle = g_window;
#endif
    config.rsp_callbacks = recomp::rsp::callbacks_t{
        .get_rsp_microcode = get_rsp_microcode,
    };
    config.renderer_callbacks = ultramodern::renderer::callbacks_t{
        .create_render_context = bumble::rt64_renderer::create,
        .get_graphics_api_name = nullptr,
    };
    config.audio_callbacks = ultramodern::audio_callbacks_t{
        .queue_samples = bumble::native_io::queue_samples,
        .get_frames_remaining = bumble::native_io::get_frames_remaining,
        .set_frequency = bumble::native_io::set_frequency,
    };
    config.input_callbacks = ultramodern::input::callbacks_t{
        .poll_input = bumble::native_io::poll_input,
        .get_input = bumble::native_io::get_input,
        .set_rumble = bumble::native_io::set_rumble,
        .get_connected_device_info = bumble::native_io::get_connected_device_info,
    };
    config.gfx_callbacks = ultramodern::gfx_callbacks_t{
        .create_gfx = nullptr,
        .create_window = nullptr,
        .update_gfx = pump_messages,
    };
    RT64::ProfilerGuestVI::install();
    config.events_callbacks = ultramodern::events::callbacks_t{
        .vi_callback = profile_vi_tick,
        .gfx_init_callback = nullptr,
        .graphics_action_dequeued = profile_graphics_action_dequeued,
        .graphics_action_completed = profile_graphics_action_completed,
    };
    config.message_queue_control = ultramodern::MessageQueueControl{};
    log_stage("message_queue_policy timer=1 sp=1 si=1 ai=0 vi=0 pi=0 dp=1");

    log_stage("runtime_start_enter");
    recomp::start(config);
    log_stage("runtime_start_returned");

    const bool replay_failed = bumble::native_io::replay_configured() &&
        (!bumble::native_io::replay_complete() ||
         !bumble::native_io::replay_semantic_success() ||
         !bumble::native_checkpoint::widescreen_hud_validation_complete());

    destroy_visible_window();
    bumble::level_editor::shutdown();
    bumble::graphics_options::shutdown();
    bumble::native_io::shutdown();
    bumble::controller_pak::reset_runtime_state();
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=clean_shutdown input_polls=%llu input_transitions=%llu "
        "audio_samples_queued=%llu audio_samples_consumed=%llu display_lists=%" PRIu32
        " screen_updates=%" PRIu32
        " intro_skips=%llu"
        " rumble_prompt_bypasses=%llu"
        " modern_player_frames=%llu modern_aim_updates=%llu"
        " modern_movement_frames=%llu"
        " replay_configured=%d replay_complete=%d replay_semantic_success=%d"
        " widescreen_hud_validation_complete=%d"
        " replay_tick=%llu replay_game_events=%" PRIu32 "/%" PRIu32 "\n",
        static_cast<unsigned long long>(bumble::native_io::input_poll_count()),
        static_cast<unsigned long long>(bumble::native_io::input_transition_count()),
        static_cast<unsigned long long>(bumble::native_io::queued_audio_sample_count()),
        static_cast<unsigned long long>(bumble::native_io::consumed_audio_sample_count()),
        bumble::rt64_renderer::display_list_count(),
        bumble::rt64_renderer::screen_update_count(),
        static_cast<unsigned long long>(
            bumble::startup_flow::intro_skip_count()
        ),
        static_cast<unsigned long long>(
            bumble::startup_flow::rumble_prompt_bypass_count()
        ),
        static_cast<unsigned long long>(
            bumble::modern_controls::active_player_frame_count()
        ),
        static_cast<unsigned long long>(
            bumble::modern_controls::player_aim_update_count()
        ),
        static_cast<unsigned long long>(
            bumble::modern_controls::movement_frame_count()
        ),
        bumble::native_io::replay_configured() ? 1 : 0,
        bumble::native_io::replay_complete() ? 1 : 0,
        bumble::native_io::replay_semantic_success() ? 1 : 0,
        bumble::native_checkpoint::widescreen_hud_validation_complete() ? 1 : 0,
        static_cast<unsigned long long>(bumble::native_io::replay_tick()),
        bumble::native_io::replay_completed_game_event_count(),
        bumble::native_io::replay_expected_game_event_count()
    );
    return replay_failed ? 10 : 0;
}
