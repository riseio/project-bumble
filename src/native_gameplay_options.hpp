#pragma once

#include <cstdint>

namespace bumble::graphics_options {

bool all_weapons_enabled();
bool unlimited_ammo_enabled();
bool unlimited_health_enabled();
bool honeycomb_water_rescue_enabled();
bool double_ammo_pickups_enabled();
bool double_mission_time_limits_enabled();
bool double_enemy_health_enabled();
bool double_enemy_awareness_enabled();
bool a_d_strafing_enabled();
bool unlock_all_levels_enabled();
bool honeycomb_health_enabled();
bool half_player_health_enabled();
uint32_t campaign_unlocked_level();
bool record_campaign_progress(uint32_t next_level);
bool interactive_menu_enabled();
void request_play_menu_return();

} // namespace bumble::graphics_options
