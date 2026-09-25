#pragma once

#include <filesystem>
#include <atomic>
#include <cstdint>
#include <vector>
#include <string_view>

namespace bumble::first_run {
bool validate_texture_archive(const std::filesystem::path& path,
    const std::atomic_bool* cancelled = nullptr);
inline constexpr std::string_view kAssetGenerator = "bumble-native-assets-v6";
inline constexpr std::string_view kFaithfulTextureFormat = "R8G8B8A8_UNORM_point_mips";

std::vector<uint8_t> make_faithful_dds(std::vector<uint8_t> pixels,
    uint32_t width, uint32_t height, uint32_t mip_count);

enum class AssetResult { Ready, Cancelled, Failed };
bool enhanced_textures_available();
bool texture_prompt_requested();
bool request_texture_prompt();
AssetResult ensure_assets(
    const std::filesystem::path& data_root,
    bool unattended = false
);

} // namespace bumble::first_run
