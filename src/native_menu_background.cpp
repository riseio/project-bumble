#include "native_menu_background.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <string>

#include "recomp.h"

namespace {

constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kFrontendPhaseOffset = 0x84u;
constexpr uint32_t kMainMenuPhase = 0x0000000Cu;
constexpr uint32_t kHighResolutionBackgroundStripCount = 120u;
constexpr uint32_t kSuppressedGuestLogoTextureCount = 7u;
constexpr uint32_t kHighResolutionGameplayUiTextureCount = 8u;
constexpr uint32_t kExpectedReplacementCount =
    kHighResolutionBackgroundStripCount +
    kSuppressedGuestLogoTextureCount +
    kHighResolutionGameplayUiTextureCount;
constexpr double kStandardAspect = 4.0 / 3.0;
constexpr double kWidescreenAspect = 16.0 / 9.0;

std::filesystem::path g_asset_root;
std::mutex g_configuration_mutex;
std::atomic_bool g_configured{false};
std::atomic_bool g_menu_latched{false};
std::atomic_uint64_t g_activation_count{0};

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t read_u32(const uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

bool validate_pack(
    const std::filesystem::path& asset_root,
    const char* variant
) {
    const std::filesystem::path pack = asset_root / variant;
    const std::filesystem::path database = pack / "rt64.json";
    if (!std::filesystem::is_regular_file(database) ||
        std::filesystem::file_size(database) == 0) {
        return false;
    }

    uint32_t png_count = 0;
    for (const auto& item : std::filesystem::directory_iterator(pack)) {
        if (item.is_regular_file() && item.path().extension() == ".png") {
            ++png_count;
        }
    }

    return png_count == kExpectedReplacementCount;
}

} // namespace

bool bumble::menu_background::configure(
    const std::filesystem::path& asset_root
) {
    g_configured.store(false, std::memory_order_release);
    g_menu_latched.store(false, std::memory_order_release);
    g_activation_count.store(0, std::memory_order_release);

    const std::filesystem::path manifest = asset_root / "manifest.json";
    const bool valid =
        std::filesystem::is_regular_file(manifest) &&
        std::filesystem::file_size(manifest) > 0 &&
        validate_pack(asset_root, "standard_4x3") &&
        validate_pack(asset_root, "widescreen_16x9");
    if (!valid) {
        std::fprintf(
            stderr,
            "BUMBLE_MENU_BACKGROUND stage=asset_invalid path=%s"
            " expected_variants=2 expected_replacements_per_variant=%" PRIu32
            "\n",
            asset_root.string().c_str(),
            kExpectedReplacementCount
        );
        std::fflush(stderr);
        return false;
    }

    {
        const std::lock_guard lock(g_configuration_mutex);
        g_asset_root = asset_root;
    }
    g_configured.store(true, std::memory_order_release);
    return true;
}

bool bumble::menu_background::configured() {
    return g_configured.load(std::memory_order_acquire);
}

bumble::menu_background::Variant
bumble::menu_background::variant_for_window(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return Variant::Widescreen16x9;
    }

    const double aspect = static_cast<double>(width) /
        static_cast<double>(height);
    const double standard_distance =
        aspect > kStandardAspect
            ? aspect - kStandardAspect
            : kStandardAspect - aspect;
    const double widescreen_distance =
        aspect > kWidescreenAspect
            ? aspect - kWidescreenAspect
            : kWidescreenAspect - aspect;
    return standard_distance < widescreen_distance
        ? Variant::Standard4x3
        : Variant::Widescreen16x9;
}

const char* bumble::menu_background::variant_name(Variant variant) {
    switch (variant) {
    case Variant::Standard4x3:
        return "standard_4x3";
    case Variant::Widescreen16x9:
        return "widescreen_16x9";
    }
    return "widescreen_16x9";
}

std::filesystem::path bumble::menu_background::replacement_directory(
    Variant variant
) {
    const std::lock_guard lock(g_configuration_mutex);
    return g_asset_root / variant_name(variant);
}

bool bumble::menu_background::observe_phase(const uint8_t* rdram) {
    if (!configured() || rdram == nullptr) {
        return false;
    }

    const uint32_t phase =
        read_u32(rdram, kFrontendObject + kFrontendPhaseOffset);
    const bool main_menu = phase == kMainMenuPhase;
    if (!main_menu) {
        g_menu_latched.store(false, std::memory_order_release);
        return false;
    }

    bool expected = false;
    if (g_menu_latched.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        const uint64_t count =
            g_activation_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::fprintf(
            stderr,
            "BUMBLE_MENU_BACKGROUND stage=menu_active count=%" PRIu64
            " phase=0x%08" PRIX32
            " composition=project_background_rt64"
            " background_strips=120 guest_rom_logo_suppressed=1"
            " transparent_logo_tiles=7"
            " guest_text=native_embedded_roboto"
            " guest_writes=0\n",
            count,
            kMainMenuPhase
        );
        std::fflush(stderr);
    }

    return true;
}

uint64_t bumble::menu_background::activation_count() {
    return g_activation_count.load(std::memory_order_acquire);
}
