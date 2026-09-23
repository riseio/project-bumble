#pragma once

#include <cstdint>
#include <vector>
#include <json/json.hpp>

namespace bumble::textures {
nlohmann::json enhanced_catalog(const std::vector<uint8_t>& rom, nlohmann::json textures);
}
