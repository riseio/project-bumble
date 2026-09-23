#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::widescreen {

void publish_render_size(uint32_t width, uint32_t height, bool enabled);

void publish_fog_scale(float scale);
float fog_scale();

double horizontal_expansion_scale();

bool visibility_expansion_enabled();

bool extended_ui_enabled();
void invalidate_world_camera_history();
void invalidate_scene_history();

struct VisibilityCameraXZ {
    float eye_x = 0.0f;
    float eye_z = 0.0f;
    float cull_x = 0.0f;
    float cull_z = 0.0f;
    float forward_x = 0.0f;
    float forward_z = 0.0f;
    uint64_t generation = 0;
    bool valid = false;
};

VisibilityCameraXZ visibility_camera_xz();

struct TerrainVisibilitySample {
    uint32_t original_cells = 0u;
    uint32_t candidate_cells = 0u;
    uint32_t visible_cells = 0u;
    int32_t first_row = 0;
    int32_t last_row = 0;
    uint64_t generation = 0u;
    bool valid = false;
};

TerrainVisibilitySample terrain_visibility_sample();

uint32_t display_list_frame_base();

uint32_t display_list_arena_base();
uint32_t display_list_arena_end();

uint32_t matrix_arena_capacity();

uint64_t legacy_level_entry_suppression_count();
uint64_t post_commit_level_select_suppression_count();

} // namespace bumble::widescreen

extern "C" void bumble_prepare_extended_matrix_arena(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_restore_task_display_list_owner(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_guard_extended_display_list_capacity(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_begin_actor_matrix_group(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_end_actor_matrix_group(uint8_t* rdram);

extern "C" void bumble_configure_single_player_frame_scissor(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_configure_single_player_depth_clear(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_force_wide_sky_background_fill(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_configure_single_player_color_clear(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_configure_single_player_world_aperture(
    uint8_t* rdram,
    recomp_context* context
) noexcept(false);
extern "C" void bumble_restore_split_screen_world_aperture(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_expand_terrain_visibility(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_relocate_terrain_visibility_build_scratch(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_relocate_terrain_visibility_consume_scratch(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_finish_terrain_visibility_scratch(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_ui_begin_fullscreen_panel(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_fullscreen_panel(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_texrect(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_end_texrect(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_setup_2d_scissor(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_observe_briefing_queue_text(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_script_panel(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_script_panel(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_script_text(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_script_text(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_briefing_panel(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_briefing_panel(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_expand_briefing_wrap_width(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_briefing_line(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t call_pc
);
extern "C" void bumble_ui_end_briefing_line(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_text(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_reduce_main_menu_pulse(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_text(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_begin_text_line(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_end_text_line(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_begin_native_menu_list(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_native_menu_list(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_direct_text(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_end_direct_text(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_begin_sprite_rect(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_end_sprite_rect(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_begin_hud_rect_batch(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_end_hud_rect_batch(uint8_t* rdram, recomp_context* context);
extern "C" void bumble_ui_begin_gameplay_hud_root(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_hud_root(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_radar(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_radar(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_weapon_model(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_weapon_model(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_key_group(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_key_group(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_weapon_neighbor(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_weapon_neighbor(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_right_hud(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_right_hud(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_lives(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_lives(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_campaign_level_grid(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_campaign_level_grid(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_weapon_ammo(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_weapon_ammo(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_status_bar(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_mission_gauge(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_mission_gauge(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_status_bar(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_text(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_text(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_number(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_number(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_begin_gameplay_timer(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_ui_end_gameplay_timer(
    uint8_t* rdram,
    recomp_context* context
);
