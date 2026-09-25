#pragma once

#include <array>
#include <cstdint>

#include "native_weapon_system.hpp"
#include "recomp.h"

namespace bumble::native_checkpoint {
bool validate_player_lifecycle_contracts();

struct RuntimePerformanceCounts {
    uint32_t active_actors = 0;
    uint32_t rendered_actors = 0;
    uint32_t particle_actors = 0;
    std::array<uint32_t, weapon_system::kActorFamilyCount> weapon_actors{};
    float player_x = 0.0f;
    float player_y = 0.0f;
    float player_z = 0.0f;
    float player_yaw = 0.0f;
};

enum class MissionRestart : uint32_t {
    Beginning,
    LastCheckpoint,
};

bool mission_checkpoint_available();
void capture_portal_mission_checkpoint(
    uint8_t* rdram,
    recomp_context* context
);
void request_mission_restart(MissionRestart restart);
bool apply_pending_mission_restart(uint8_t* rdram, recomp_context* context);

uint32_t frontend_confirm_count();
uint32_t last_frontend_phase();
uint32_t last_accepted_frontend_phase();
uint32_t last_frontend_descriptor();
uint32_t last_frontend_descriptor_item();
uint32_t level_select_cheat_progress();
bool level_select_cheat_complete();
bool mission2_selector_initialized();
bool mission2_selected();
bool mission2_selection_committed();
uint32_t campaign_selector_index();
uint32_t campaign_selection_committed_index();
bool campaign_grid_active();
uint32_t campaign_grid_selected_slot();
bool campaign_grid_back_hovered();
void retire_campaign_grid();
uint32_t campaign_grid_progress_level();
uint32_t campaign_player_level_observed();
bool mission1_player_observed();
bool mission2_player_observed();
void configure_mission1_completion_replay(bool enabled, bool withhold_one_owner);
bool mission1_completion_replay_enabled();
uint16_t mission1_completion_buttons_requested();
uint32_t mission1_installer_count();
uint32_t mission1_registered_owner_count();
uint32_t mission1_terminal_count();
uint32_t mission1_counter_value();
uint32_t mission1_delay_value();
bool mission1_success_callback_observed();
bool mission1_success_frontend_observed();
bool mission1_level_increment_observed();
bool mission1_profile_save_observed();
bool mission1_profile_reopen_observed();
void configure_widescreen_hud_validation(bool enabled);
uint32_t take_widescreen_hud_capture_request();
void mark_widescreen_hud_capture_complete(uint32_t selected_weapon);
bool widescreen_hud_validation_complete();
bool two_player_state2_ready();
void arm_two_player_control_observation(bool positive_control);
bool two_player_checkpoint_observed();
bool two_player_positive_control_observed();
bool half_player_health_active();
RuntimePerformanceCounts runtime_performance_counts();

} // namespace bumble::native_checkpoint
