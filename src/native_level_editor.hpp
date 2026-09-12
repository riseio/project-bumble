#pragma once

#include <cstdint>
#include <filesystem>

#include "recomp.h"

namespace bumble::level_editor {

struct Preview {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float forward_x = 0.0f;
    float forward_z = 1.0f;
    float radius = 18.0f;
    uint32_t actor = 0u;
    bool delete_target = false;
    bool valid = false;
};

bool initialize(
    const std::filesystem::path& data_root,
    const std::filesystem::path& rom_path,
    bool enabled
);
void shutdown();

bool menu_open();
bool active();
bool handle_key(uint32_t virtual_key, bool pressed);
bool handle_primary_mouse(bool pressed);
void release_input_state();
bool preview(Preview& value);

} // namespace bumble::level_editor

extern "C" void bumble_level_editor_observe_authored_record(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_level_editor_preserve_louse_actor();

extern "C" uint32_t bumble_level_editor_tick(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_level_editor_skip_actor_update(
    uint8_t* rdram,
    uint32_t update_subobject
);
extern "C" void bumble_level_editor_finish_actor_update(uint8_t* rdram);
