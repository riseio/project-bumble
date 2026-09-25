#pragma once

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

void bumble_observe_web_box_contact(uint8_t* rdram, recomp_context* ctx);
void bumble_forget_web_box_contact(uint32_t box);
void bumble_clear_web_box_contacts(void);
void bumble_begin_damage_feedback(uint8_t*, recomp_context*);
void bumble_end_damage_feedback(uint8_t*, recomp_context*, uint32_t stack_size);
void bumble_bind_boss_encounter(uint8_t*, recomp_context*);
void bumble_begin_boss_health_bar(uint8_t*, recomp_context*);
void bumble_end_boss_health_bar(uint8_t*, recomp_context*);

#define osPfsInitPak_recomp buck_osPfsInitPak_recomp
#define osPfsFreeBlocks_recomp buck_osPfsFreeBlocks_recomp
#define osPfsAllocateFile_recomp buck_osPfsAllocateFile_recomp
#define osPfsDeleteFile_recomp buck_osPfsDeleteFile_recomp
#define osPfsFileState_recomp buck_osPfsFileState_recomp
#define osPfsFindFile_recomp buck_osPfsFindFile_recomp
#define osPfsReadWriteFile_recomp buck_osPfsReadWriteFile_recomp

void buck_osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_controller_pak_validate_recomp(uint8_t* rdram, recomp_context* ctx);

void buck_sound_command_dispatch_recomp(uint8_t* rdram, recomp_context* ctx);
void buck_resource_command_dispatch_recomp(uint8_t* rdram, recomp_context* ctx);

void osContReset_recomp(uint8_t* rdram, recomp_context* ctx);

void buck_osSetEventMesg_bridge(
    uint8_t* rdram,
    uint32_t event_id,
    int32_t queue,
    int32_t message
);

void buck_raw_pad_return_probe(uint8_t* rdram, recomp_context* ctx);
void buck_controller_init_probe(uint8_t* rdram, recomp_context* ctx);
void buck_normalized_pad_probe(uint8_t* rdram, recomp_context* ctx);
void buck_frontend_confirm_probe(uint8_t* rdram, recomp_context* ctx);
void buck_level_select_cheat_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_capture_campaign_level_progress(uint8_t* rdram, recomp_context* ctx);
void bumble_initialize_campaign_level_grid(uint8_t* rdram, recomp_context* ctx);
void bumble_handle_campaign_level_grid_input(uint8_t* rdram, recomp_context* ctx);
void bumble_commit_campaign_level_grid(uint8_t* rdram, recomp_context* ctx);
void buck_mission2_selector_init_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission2_selector_select_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission2_selection_commit_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_update_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_level_editor_observe_authored_record(
    uint8_t* rdram,
    recomp_context* ctx
);
uint32_t bumble_level_editor_preserve_louse_actor(void);
void bumble_level_editor_repair_missing_target(
    uint8_t* rdram,
    recomp_context* ctx
);
uint32_t bumble_level_editor_tick(uint8_t* rdram, recomp_context* ctx);
uint32_t bumble_level_editor_skip_actor_update(
    uint8_t* rdram,
    uint32_t update_subobject
);
void bumble_level_editor_finish_actor_update(uint8_t* rdram);
void bumble_enemy_spawner_retire_collision(
    uint8_t* rdram,
    uint32_t actor
);
void buck_mission1_installer_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_child_target_probe(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t source_pc,
    uint32_t actor_register
);
void buck_mission1_terminal_caller_probe(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t caller_pc
);
void buck_mission1_damage_event_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_terminal_entry_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_terminal_exit_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_decrement_store_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_zero_gate_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_success_callback_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_handle_mission_outcome_checkpoint(
    uint8_t* rdram,
    recomp_context* ctx
);
int bumble_restore_imminent_mission_failure_checkpoint(
    uint8_t* rdram,
    recomp_context* ctx
);
void buck_mission1_success_frontend_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_level_increment_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_autosave_campaign_progress(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_profile_save_entry_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_profile_built_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_profile_save_result_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_profile_reopen_entry_probe(uint8_t* rdram, recomp_context* ctx);
void buck_mission1_profile_reopen_result_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_mission1_completion_player_update(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_player_maximum_health(uint8_t* rdram, recomp_context* ctx);
void bumble_cancel_life_decrement(uint8_t* rdram, recomp_context* ctx);
void bumble_remove_lives_gate(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_damage_immunity(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_honeycomb_water_rescue(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_ammo_pickup_amount(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_mission_time_limit(uint8_t* rdram, recomp_context* ctx);
void bumble_gameplay_modifier_tick(uint8_t* rdram);
void bumble_record_rendered_actor(void);
void bumble_begin_actor_matrix_group(uint8_t* rdram, recomp_context* ctx);
void bumble_register_actor_history(uint8_t* rdram, recomp_context* ctx);
void bumble_unregister_actor_history(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
void bumble_begin_model_part(uint8_t* rdram, recomp_context* ctx) noexcept(false);
void bumble_end_model_part(uint8_t* rdram) noexcept(false);
#else
void bumble_begin_model_part(uint8_t* rdram, recomp_context* ctx);
void bumble_end_model_part(uint8_t* rdram);
#endif
void bumble_set_model_role(uint32_t role);
void bumble_wait_graphics_input(uint8_t* rdram, uint32_t waiter_index);
void bumble_end_actor_matrix_group(uint8_t* rdram);
void bumble_scale_enemy_awareness_planar_distance(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_scale_enemy_awareness_target_distance(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_scale_enemy_awareness_player_distance(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_apply_enemy_health_damage(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t actor
);
void buck_player_one_owner_commit_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_two_owner_commit_probe(uint8_t* rdram, recomp_context* ctx);
void buck_generic_player_callback_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state_dispatch_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state2_control_sample_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state2_scaled_controls_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state2_orientation_sample_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state2_orientation_post_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state2_integrator_pre_probe(uint8_t* rdram, recomp_context* ctx);
void buck_player_state2_integrator_post_probe(uint8_t* rdram, recomp_context* ctx);

void bumble_apply_startup_menu_skip(uint8_t* rdram, recomp_context* ctx);

void bumble_bypass_new_game_rumble_prompt(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_disable_main_menu_attract(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_preempt_main_menu_attract_transition(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_retain_main_menu_after_attract_transition(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_preempt_new_game_rumble_prompt(
    uint8_t* rdram,
    recomp_context* ctx
);

void bumble_update_modern_player_aim(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_modern_player_aim(uint8_t* rdram, recomp_context* ctx);
uint32_t bumble_apply_modern_guided_missile_aim(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_begin_modern_barrel_roll_visual(uint8_t* rdram, recomp_context* ctx);
void bumble_end_modern_barrel_roll_visual(uint8_t* rdram, recomp_context* ctx);
void bumble_gate_modern_player_teleport(uint8_t* rdram, recomp_context* ctx);
void bumble_complete_modern_player_teleport(uint8_t* rdram, recomp_context* ctx);
void bumble_prepare_modern_teleport_visual_record(uint8_t* rdram, recomp_context* ctx);
void bumble_tag_modern_teleport_visual_record(uint8_t* rdram, recomp_context* ctx);
void bumble_begin_modern_teleport_visual_scope(uint8_t* rdram, recomp_context* ctx);
void bumble_end_modern_teleport_visual_scope(uint8_t* rdram, recomp_context* ctx);
void bumble_register_dynamic_gate_layer(uint8_t* rdram, uint32_t actor);
void bumble_prepare_modern_collision_world(uint8_t* rdram, recomp_context* ctx);
void bumble_prepare_modern_player_movement(uint8_t* rdram, recomp_context* ctx);
uint32_t bumble_step_modern_player_airborne(uint8_t* rdram, recomp_context* ctx);
uint32_t bumble_step_modern_player_grounded(uint8_t* rdram, recomp_context* ctx);
uint32_t bumble_continue_modern_player_collision_update(uint8_t* rdram, recomp_context* ctx);
void bumble_finish_modern_player_collision_update(uint8_t* rdram, recomp_context* ctx);
void bumble_resolve_modern_airborne_collision(uint8_t* rdram, recomp_context* ctx);
void bumble_gate_manual_landing(uint8_t* rdram, recomp_context* ctx);
void bumble_retract_modern_airborne_landing_gear(uint8_t* rdram, recomp_context* ctx);
void bumble_stabilize_modern_airborne_collision_recovery(uint8_t* rdram, recomp_context* ctx);
void bumble_guard_manual_landing_state_store(uint8_t* rdram, recomp_context* ctx);
void bumble_guard_manual_grounded_entry_state_store(uint8_t* rdram, recomp_context* ctx);
void bumble_observe_automatic_ground_exit(uint8_t* rdram, recomp_context* ctx);
void bumble_observe_automatic_ground_exit_commit(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_manual_takeoff_press(uint8_t* rdram, recomp_context* ctx);
void bumble_bypass_manual_takeoff_state7_exclusion(uint8_t* rdram, recomp_context* ctx);
void bumble_bypass_manual_takeoff_resource_gate(uint8_t* rdram, recomp_context* ctx);
void bumble_observe_manual_takeoff_commit(uint8_t* rdram, recomp_context* ctx);
void bumble_mark_modern_gameplay_active(uint8_t* rdram, recomp_context* ctx);
void bumble_mark_modern_gameplay_inactive(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_modern_translated_confirm(uint8_t* rdram, recomp_context* ctx);
void bumble_apply_modern_frontend_confirm(uint8_t* rdram, recomp_context* ctx);
void bumble_handle_native_game_completion_input(
    uint8_t* rdram,
    recomp_context* ctx
);

void bumble_update_multi_bomb_static_contact(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t actor
);
void bumble_begin_weapon_static_step(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t actor,
    uint32_t weapon_slot
);
void bumble_prepare_projectile_scene_range(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t actor,
    uint32_t weapon_slot
);
uint32_t bumble_resolve_weapon_static_contact(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t actor,
    uint32_t weapon_slot,
    uint32_t authored_contact
);
uint32_t bumble_update_modern_lightning_projectile(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t actor
);
uint32_t bumble_lightning_collision_target_visible(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t source,
    uint32_t collision_record
);

void bumble_prepare_native_graphics_options_menu(uint8_t* rdram, recomp_context* ctx);
void bumble_handle_native_graphics_options_input(uint8_t* rdram, recomp_context* ctx);

void bumble_expand_terrain_visibility(uint8_t* rdram, recomp_context* ctx);
void bumble_relocate_terrain_visibility_build_scratch(uint8_t* rdram, recomp_context* ctx);
void bumble_relocate_terrain_visibility_consume_scratch(uint8_t* rdram, recomp_context* ctx);
void bumble_finish_terrain_visibility_scratch(uint8_t* rdram, recomp_context* ctx);
void bumble_collect_procedural_grass_cell(uint8_t* rdram, recomp_context* ctx);
void bumble_emit_procedural_grass_pass(uint8_t* rdram, recomp_context* ctx);
void bumble_emit_modern_sky_pass(uint8_t* rdram, recomp_context* ctx);
void bumble_emit_modern_electric_pass(
    uint8_t* rdram,
    recomp_context* ctx
);

void bumble_prepare_extended_matrix_arena(uint8_t* rdram, recomp_context* ctx);
void bumble_guard_extended_display_list_capacity(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_restore_task_display_list_owner(
    uint8_t* rdram,
    recomp_context* ctx
);

void bumble_configure_single_player_frame_scissor(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_configure_single_player_depth_clear(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_force_wide_sky_background_fill(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_configure_single_player_color_clear(
    uint8_t* rdram,
    recomp_context* ctx
);
#ifdef __cplusplus
void bumble_configure_single_player_world_aperture(uint8_t* rdram, recomp_context* ctx) noexcept(false);
void bumble_end_world_camera_scope(uint8_t* rdram, recomp_context* ctx) noexcept(false);
#else
void bumble_configure_single_player_world_aperture(uint8_t* rdram, recomp_context* ctx);
void bumble_end_world_camera_scope(uint8_t* rdram, recomp_context* ctx);
#endif
void bumble_invalidate_world_camera_history(uint8_t* rdram, recomp_context* ctx);
void bumble_publish_script_camera_history(uint8_t* rdram, recomp_context* ctx);
void bumble_restore_split_screen_world_aperture(uint8_t* rdram, recomp_context* ctx);

void bumble_object_cull_pvs_reject_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_expand_static_geometry_pvs(uint8_t* rdram, recomp_context* ctx);
void bumble_object_cull_distance_reject_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_scale_object_distance_threshold(uint8_t* rdram, recomp_context* ctx);
void bumble_guard_object_matrix_capacity(uint8_t* rdram, recomp_context* ctx);
void bumble_guard_post_object_matrix_capacity(uint8_t* rdram, recomp_context* ctx);
void bumble_matrix_arena_world_pass_probe(uint8_t* rdram, recomp_context* ctx);
void bumble_record_modern_particle_spawn(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_mark_next_modern_particle(uint32_t mode);
void bumble_mark_next_modern_legacy_effect(uint32_t mode);
void bumble_record_modern_legacy_effect_spawn(
    uint8_t* rdram,
    recomp_context* ctx
);
void bumble_record_actor_modern_explosion(
    uint8_t* rdram,
    uint32_t actor,
    float scale
);
void bumble_record_laser_impact(uint8_t* rdram, uint32_t actor);
void bumble_begin_native_high_scores(uint8_t* rdram, recomp_context* ctx);
void bumble_end_native_high_scores(uint8_t* rdram, recomp_context* ctx);
void bumble_record_debris_modern_explosion(
    uint8_t* rdram,
    recomp_context* ctx
);
uint32_t bumble_replace_modern_particle_render(
    uint8_t* rdram,
    recomp_context* ctx
);
uint32_t bumble_replace_modern_legacy_effect_render(
    uint8_t* rdram,
    recomp_context* ctx
);

void bumble_ui_begin_texrect(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_texrect(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_setup_2d_scissor(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_reduce_main_menu_pulse(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_text_line(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_text_line(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_native_menu_list(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_native_menu_list(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_observe_briefing_queue_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_script_panel(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_script_panel(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_script_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_script_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_briefing_panel(uint8_t* rdram, recomp_context* ctx);
void bumble_reset_briefing_text(uint8_t* rdram, recomp_context* ctx);
void bumble_queue_briefing_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_briefing_panel(uint8_t* rdram, recomp_context* ctx);
void bumble_expand_briefing_wrap_width(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_briefing_line(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t call_pc
);
void bumble_ui_end_briefing_line(uint8_t* rdram, recomp_context* ctx);
// Capture the call PC before entry: direct AOT calls do not set guest r31.
void bumble_ui_publish_direct_text_call(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t call_pc
);
void bumble_ui_begin_direct_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_direct_text(uint8_t* rdram, recomp_context* ctx);
void bumble_death_screen_observe_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_sprite_rect(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_sprite_rect(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_hud_rect_batch(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_hud_rect_batch(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_hud_root(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_hud_root(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_radar(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_radar(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_weapon_model(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_weapon_model(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_weapon_neighbor(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_weapon_neighbor(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_right_hud(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_right_hud(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_lives(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_lives(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_campaign_level_grid(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_campaign_level_grid(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_weapon_ammo(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_weapon_ammo(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_status_bar(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_mission_gauge(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_mission_gauge(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_status_bar(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_text(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_gameplay_number(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_gameplay_number(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_begin_fullscreen_panel(uint8_t* rdram, recomp_context* ctx);
void bumble_ui_end_fullscreen_panel(uint8_t* rdram, recomp_context* ctx);

void buck_native_pi_dma_post_probe(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t call_site
);
void buck_native_pi_dma_receipt_probe(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t receipt_pc
);

void buck_native_rsp_task_probe(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t anchor_pc
);
void buck_si_device_busy_clear(uint8_t* rdram, recomp_context* ctx);
void bumble_scale_cutscene_text_delay(uint8_t* rdram, recomp_context* ctx);
void bumble_retire_player_lifetime(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif
