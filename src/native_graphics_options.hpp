#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
struct SDL_Window;
#endif

#include "recomp.h"
#include "native_gameplay_options.hpp"
#include "native_visible_ui_state.hpp"
#include "render/rt64_bumble_environment.h"
#include "ultramodern/config.hpp"

namespace bumble::graphics_options {

enum class DisplayMode : uint32_t {
    BorderedWindow = 0,
    Fullscreen = 1,
};

enum class AspectMode : uint32_t {
    Standard4x3 = 0,
    Widescreen = 1,
};

enum class FogMode : uint32_t {
    None = 0,
    Half = 1,
    Full = 2,
};

enum class FramePacing : uint32_t {
    Original30Hz = 0,
    Interpolated60Hz = 1,
    Interpolated120Hz = 2,
};

enum class GrassMode : uint32_t {
    Off = 0,
    Low = 1,
    High = 2,
};

struct Settings {
    DisplayMode display_mode = DisplayMode::BorderedWindow;
    uint32_t resolution_width = 1280;
    uint32_t resolution_height = 720;
    AspectMode aspect_mode = AspectMode::Widescreen;
    FramePacing frame_pacing = FramePacing::Interpolated120Hz;
    FogMode fog_mode = FogMode::Full;
    bool high_resolution_textures = true;
    bool hd_terrain = true;
    bool modern_lighting = true;
    bool enhanced_textures = false;
    GrassMode grass_mode = GrassMode::Off;
    bool collision_overlay = false;
    bool all_weapons = false;
    bool unlimited_ammo = false;
    bool unlimited_health = false;
    bool honeycomb_water_rescue = false;
    bool double_ammo_pickups = false;
    bool double_mission_time_limits = false;
    bool double_enemy_health = false;
    bool double_enemy_awareness = false;
    bool a_d_strafing = true;
    bool unlock_all_levels = false;
    bool honeycomb_health = false;
    bool half_player_health = false;
};

static_assert(!Settings{}.all_weapons && !Settings{}.unlimited_ammo &&
    !Settings{}.unlimited_health && !Settings{}.unlock_all_levels &&
    !Settings{}.collision_overlay && !Settings{}.honeycomb_water_rescue &&
    !Settings{}.double_ammo_pickups && !Settings{}.double_mission_time_limits &&
    !Settings{}.double_enemy_health && !Settings{}.double_enemy_awareness &&
    !Settings{}.honeycomb_health && !Settings{}.half_player_health,
    "Release gameplay cheats and test modifiers must default off");

bool initialize(const std::filesystem::path& config_root);
void shutdown();

Settings current();
void apply(const Settings& settings);
std::vector<std::pair<uint32_t, uint32_t>> resolution_choices();
uint32_t initial_client_width();
uint32_t initial_client_height();
bool widescreen_enabled();
FramePacing frame_pacing();
bool high_resolution_textures_enabled();
bool hd_terrain_enabled();
bool modern_lighting_enabled();
bool enhanced_textures_enabled();
void toggle_modern_visuals();
void publish_lighting_environment(uint8_t* rdram);
GrassMode grass_mode();
bool ray_traced_lighting_enabled();
float fog_scale();

void apply_to_graphics_config(ultramodern::renderer::GraphicsConfig& config);

struct PointerSnapshot {
    int32_t client_x = -1;
    int32_t client_y = -1;
    uint32_t client_width = 0u;
    uint32_t client_height = 0u;
    uint64_t motion_revision = 0u;
    uint64_t click_revision = 0u;
    bool inside_client = false;
    bool click_down = false;
};

bool mouse_menu_navigation_active();
void stage_pause_menu_transition();
void update_menu_pointer(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    bool click,
    bool inside
);
void resize_menu_pointer(uint32_t width, uint32_t height);
void release_menu_pointer_click();
void clear_menu_pointer();
PointerSnapshot pointer_snapshot();
void set_campaign_grid_pointer_active(bool active);
bool consume_campaign_grid_pointer(uint32_t& slot, bool& click);
void mark_main_menu_handoff_ready();
void complete_main_menu_handoff();
uint64_t main_menu_handoff_completion_count();
uint8_t menu_transition_alpha();
bool runtime_shutdown_ready();

#if defined(_WIN32)
void attach_game_window(HWND window);
void notify_window_mode_applied(bool fullscreen);
bool handle_game_window_message(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    LRESULT& result
);
#else
void attach_game_window(SDL_Window* window);
void apply_pending_windowed_resolution();
void notify_window_mode_applied(bool fullscreen);
#endif

} // namespace bumble::graphics_options

extern "C" void bumble_prepare_native_graphics_options_menu(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_handle_native_graphics_options_input(
    uint8_t* rdram,
    recomp_context* context
);
