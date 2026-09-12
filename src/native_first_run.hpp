#pragma once

#include <filesystem>
#include <optional>

namespace bumble::first_run {

std::optional<std::filesystem::path> resolve_rom(
    const std::filesystem::path& data_root
);

bool remember_rom(
    const std::filesystem::path& data_root,
    const std::filesystem::path& rom_path
);

} // namespace bumble::first_run
