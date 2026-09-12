#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace bumble::campaign_levels {

inline constexpr uint32_t kRecordTable = 0x800E9FE0u;
inline constexpr uint32_t kRecordSize = 0x24u;
inline constexpr uint32_t kIdentifierSize = 20u;

struct Record {
    uint32_t level_index;
    std::string_view identifier;
    uint32_t load_offset;
    uint32_t record_word_18;
    uint32_t record_word_1c;
    uint32_t record_word_20;
};

inline constexpr std::array<Record, 24> kRecords{{
    { 1u, "01Shock Strike",      0x00005DC0u, 0x00000000u, 0x00000002u, 0x00000003u },
    { 2u, "02Radar Run",         0x0000A960u, 0x00000000u, 0x00000001u, 0x00000003u },
    { 3u, "03Return Fire",       0x0002C170u, 0x00000000u, 0x00000002u, 0x00000003u },
    { 4u, "04The Sonar Tower",   0x00008010u, 0x00000000u, 0x00000001u, 0x00000003u },
    { 5u, "05Big Blips",         0x00041800u, 0x00000000u, 0x00000002u, 0x00000003u },
    { 6u, "06Short Fuse",        0x000138F0u, 0x00000000u, 0x00000009u, 0x00000003u },
    { 7u, "07Outpost",           0x0002F3D0u, 0x00000000u, 0x0000000Cu, 0x00000003u },
    { 8u, "08Sewer",             0x0003C110u, 0x00000000u, 0x00000009u, 0x00000003u },
    { 9u, "09Clean Up",          0x0000F1D0u, 0x00000000u, 0x0000000Cu, 0x00000003u },
    {10u, "10Scramble Pylon",    0x00000DB0u, 0x00000000u, 0x00000002u, 0x00000005u },
    {11u, "11Herdling Research", 0x00024060u, 0x00000000u, 0x00000009u, 0x00000005u },
    {12u, "12The Extractor",     0x0001A650u, 0x00000000u, 0x0000000Cu, 0x00000005u },
    {13u, "13Nuke Tower",        0x00017A20u, 0x00000000u, 0x00000001u, 0x00000005u },
    {14u, "14Mucus Storage",     0x000297E0u, 0x00000000u, 0x00000010u, 0x00000003u },
    {15u, "15Depot Attack",      0x000338B0u, 0x00000001u, 0x0000000Eu, 0x00000003u },
    {16u, "88Zeppelin 2",        0x000058F0u, 0x00000028u, 0x0000000Du, 0x00000003u },
    {17u, "16Sterilization",     0x0001D1B0u, 0x00000000u, 0x00000002u, 0x00000007u },
    {18u, "17Scorpion Killer",   0x000035C0u, 0x00000000u, 0x00000010u, 0x00000008u },
    {19u, "18Core Nuke",         0x0003EB60u, 0x00000000u, 0x0000000Cu, 0x00000003u },
    {20u, "19Gatekeepers",       0x00027470u, 0x00000005u, 0x00000010u, 0x00000008u },
    {21u, "20Queen 2",           0x00000010u, 0x0000003Eu, 0x0000000Fu, 0x00000008u },
    {22u, "25Queen",             0x00000010u, 0x0000001Eu, 0x0000000Fu, 0x00000008u },
    {23u, "43Intro",             0x0001F190u, 0x00000000u, 0x00000013u, 0x00000003u },
    {25u, "46Outro",             0x00022170u, 0x00000012u, 0x0000000Bu, 0x00000014u },
}};

inline constexpr std::array<uint32_t, 19> kMissionSelectLevelIndices{{
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u,
    10u, 11u, 12u, 13u, 14u, 15u, 17u, 18u, 19u, 20u,
}};

inline constexpr uint32_t kMissionGridRows =
    static_cast<uint32_t>((kMissionSelectLevelIndices.size() + 1u) / 2u);
inline constexpr uint32_t kMissionGridFirstY = 39u;
inline constexpr uint32_t kMissionGridRowSpacing = 160u / (kMissionGridRows - 1u);
constexpr uint32_t mission_grid_column_size(uint32_t column) {
    return column == 0u ? kMissionGridRows :
        static_cast<uint32_t>(kMissionSelectLevelIndices.size()) - kMissionGridRows;
}

constexpr uint32_t record_address(uint32_t level_index) {
    return kRecordTable + level_index * kRecordSize;
}

constexpr const Record* find(uint32_t level_index) {
    for (const Record& record : kRecords) {
        if (record.level_index == level_index) {
            return &record;
        }
    }
    return nullptr;
}

constexpr uint32_t displayed_mission_number(const Record& record) {
    if (record.identifier.size() < 2u ||
        record.identifier[0] < '0' || record.identifier[0] > '9' ||
        record.identifier[1] < '0' || record.identifier[1] > '9') {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(record.identifier[0] - '0') * 10u +
        static_cast<uint32_t>(record.identifier[1] - '0');
}

constexpr bool records_are_well_formed() {
    uint32_t previous_index = 0u;
    for (const Record& record : kRecords) {
        if (record.level_index <= previous_index || record.level_index >= 32u ||
            record.identifier.empty() || record.identifier.size() > kIdentifierSize) {
            return false;
        }
        previous_index = record.level_index;
    }
    return true;
}

constexpr bool mission_selector_is_well_formed() {
    uint32_t expected_mission = 1u;
    for (const uint32_t level_index : kMissionSelectLevelIndices) {
        const Record* record = find(level_index);
        if (record == nullptr || displayed_mission_number(*record) != expected_mission) {
            return false;
        }
        ++expected_mission;
    }
    return expected_mission == kMissionSelectLevelIndices.size() + 1u;
}

static_assert(records_are_well_formed());
static_assert(mission_selector_is_well_formed());
static_assert(find(16u)->identifier == "88Zeppelin 2");
static_assert(displayed_mission_number(*find(16u)) == 88u);
static_assert(displayed_mission_number(*find(17u)) == 16u);
static_assert(find(21u)->identifier == "20Queen 2");
static_assert(find(22u)->identifier == "25Queen");
static_assert(find(25u)->identifier == "46Outro");

} // namespace bumble::campaign_levels
