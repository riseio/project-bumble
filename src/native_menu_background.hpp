#pragma once

#include <cstdint>
#include <filesystem>

namespace bumble::menu_background {

enum class Variant {
    Standard4x3,
    Widescreen16x9,
};

bool configure(const std::filesystem::path& asset_root);

bool configured();
Variant variant_for_window(uint32_t width, uint32_t height);
const char* variant_name(Variant variant);
std::filesystem::path replacement_directory(Variant variant);

bool observe_phase(const uint8_t* rdram);
uint64_t activation_count();

} // namespace bumble::menu_background
