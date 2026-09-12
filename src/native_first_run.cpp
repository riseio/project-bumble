#include "native_first_run.hpp"

#include <fstream>
#include <optional>

#include <json/json.hpp>
#include <nfd.h>

namespace {

std::filesystem::path settings_path(const std::filesystem::path& data_root) {
    return data_root / "launcher.json";
}

std::optional<std::filesystem::path> remembered_rom(
    const std::filesystem::path& data_root
) {
    try {
        std::ifstream stream(settings_path(data_root));
        const auto settings = nlohmann::json::parse(stream);
        const std::filesystem::path path =
            settings.at("rom_path").get<std::string>();
        if (std::filesystem::is_regular_file(path)) {
            return path;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> choose_rom() {
    if (NFD_Init() != NFD_OKAY) {
        return std::nullopt;
    }
    nfdu8char_t* selected = nullptr;
    const nfdu8filteritem_t filters[]{{"Nintendo 64 ROM", "z64,n64,v64"}};
    const nfdresult_t result = NFD_OpenDialogU8(&selected, filters, 1, nullptr);
    std::optional<std::filesystem::path> path;
    if (result == NFD_OKAY && selected != nullptr) {
        path = std::filesystem::path(selected);
        NFD_FreePathU8(selected);
    }
    NFD_Quit();
    return path;
}

} // namespace

std::optional<std::filesystem::path> bumble::first_run::resolve_rom(
    const std::filesystem::path& data_root
) {
    if (const auto saved = remembered_rom(data_root)) {
        return saved;
    }
    return choose_rom();
}

bool bumble::first_run::remember_rom(
    const std::filesystem::path& data_root,
    const std::filesystem::path& rom_path
) {
    try {
        std::filesystem::create_directories(data_root);
        std::ofstream stream(settings_path(data_root), std::ios::trunc);
        stream << nlohmann::json{
            {"schema_version", 1},
            {"rom_path", std::filesystem::absolute(rom_path).string()},
        }.dump(2) << '\n';
        return static_cast<bool>(stream);
    } catch (...) {
        return false;
    }
}
