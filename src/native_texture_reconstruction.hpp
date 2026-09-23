#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace bumble::textures {
struct Image { uint32_t width{}, height{}; std::vector<uint8_t> rgba; };
Image reconstruct(const Image& source, uint32_t scale = 9, const std::function<void()>& checkpoint = {});
Image restore_detail(const Image& source);
Image clean_grain(const Image& source);
Image filter_surface(const Image& reconstructed, uint32_t texel_scale = 9, const std::function<void()>& checkpoint = {});
Image reconstruct_material(const Image& source, const std::function<void()>& checkpoint = {});
std::vector<uint8_t> make_dds(const Image& image);
}
