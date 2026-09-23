#pragma once

#include <cstdint>

#include "recomp.h"

extern "C" void bumble_observe_web_box_contact(uint8_t*, recomp_context*);
extern "C" void bumble_forget_web_box_contact(uint32_t box);
extern "C" void bumble_clear_web_box_contacts();

namespace bumble::modern_controls {

struct SpatialIntent {
    float delta_x = 0.0f;
    float delta_y = 0.0f;
    float delta_z = 0.0f;
    float speed = 0.0f;
    float maneuver_progress = 0.0f;
    uint32_t maneuver_step = 0u;
    uint32_t maneuver_steps_total = 0u;
    bool landing_requested = false;
    bool takeoff_requested = false;
    bool barrel_roll_active = false;
    bool loop_de_loop_active = false;
};

void configure(bool enabled, float look_sensitivity, bool invert_look_y);
bool enabled();

void set_replay_automation(bool enabled);
bool replay_automation_enabled();

bool gameplay_input_active();
bool pause_menu_active();
bool menu_navigation_active();

void set_window_focused(bool focused);
bool window_focused();

void set_mouse_capture(bool captured);
bool mouse_captured();
void add_raw_mouse_delta(int32_t delta_x, int32_t delta_y);
void add_controller_look(float axis_x, float axis_y);
void set_primary_fire(bool pressed);
bool primary_fire_pressed();
bool frontend_confirm_pending();
void set_frontend_confirm_pressed(bool pressed);
void set_cutscene_skip_pressed(bool pressed);
void set_takeoff_land_pressed(bool pressed);
void set_loop_de_loop_pressed(bool pressed);
void set_quick_flip_pressed(bool pressed);
void set_sprint_pressed(bool pressed);
void set_barrel_roll_pressed(bool pressed);
void set_movement_input(float forward, float strafe);

bool query_spatial_intent(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t state,
    SpatialIntent& intent
);

void finish_spatial_landing_attempt(uint32_t actor, bool landed);

bool set_automation_aim(float pitch_degrees, float yaw_degrees);

uint64_t player_aim_update_count();
uint64_t movement_frame_count();
uint64_t active_player_frame_count();
bool restore_recent_collision_safe_position(uint8_t* rdram, uint32_t actor);

void rebase_after_authored_portal_transfer(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t source_teleport,
    uint32_t destination_teleport,
    uint32_t pair_id
);

void rebase_after_mission_checkpoint_restore(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t destination_teleport,
    uint32_t pair_id
);

} // namespace bumble::modern_controls

extern "C" void bumble_prepare_modern_teleport_visual_record(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_tag_modern_teleport_visual_record(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_begin_modern_teleport_visual_scope(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_end_modern_teleport_visual_scope(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_gate_modern_player_teleport(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_complete_modern_player_teleport(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_update_modern_player_aim(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_apply_modern_guided_missile_aim(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_begin_modern_barrel_roll_visual(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_end_modern_barrel_roll_visual(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_apply_modern_player_aim(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_finalize_modern_player_aim(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_prepare_modern_player_movement(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_resolve_modern_airborne_collision(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_gate_manual_landing(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_retract_modern_airborne_landing_gear(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_stabilize_modern_airborne_collision_recovery(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_guard_manual_landing_state_store(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_guard_manual_grounded_entry_state_store(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_observe_automatic_ground_exit(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_observe_automatic_ground_exit_commit(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_apply_manual_takeoff_press(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_bypass_manual_takeoff_state7_exclusion(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_bypass_manual_takeoff_resource_gate(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_observe_manual_takeoff_commit(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_mark_modern_gameplay_active(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_mark_modern_gameplay_inactive(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_apply_modern_translated_confirm(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_apply_modern_frontend_confirm(
    uint8_t* rdram,
    recomp_context* context
);
