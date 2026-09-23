#include "native_first_run_assets.hpp"
#include "native_texture_reconstruction.hpp"
#include "native_preparation.hpp"
#include "native_texture_catalog.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <json/json.hpp>
#include <miniz/miniz.h>
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "bumble_completion_background.h"
#include "bumble_death_screen.h"
#include "bumble_faithful_texture_mapping.h"
#include "bumble_gameplay_ui_mapping.h"
#include "bumble_menu_background.h"
#include "bumble_menu_hash_mapping.h"
#include "bumble_menu_panel_background.h"
#include "bumble_project_visual_assets.h"

namespace {

using json = nlohmann::json;
std::atomic_bool g_enhanced_available{false};
std::atomic_bool g_texture_prompt_requested{false};
std::filesystem::path g_texture_preference;

constexpr uint64_t kRomSize = 0xC00000u;
constexpr std::string_view kRomSha256 =
    "d21e3d1c2ec4d7f025cfaa119553be9a5fa87a9fd6625ef1ef44dc1d4b0aa54b";
constexpr std::string_view kGenerator = bumble::first_run::kAssetGenerator;
constexpr uint32_t kMenuStripCount = 120u;
constexpr uint32_t kLogoCount = 7u;
constexpr uint32_t kUiTextureCount = 8u;
constexpr uint32_t kMenuReplacementCount =
    kMenuStripCount + kLogoCount + kUiTextureCount;
constexpr uint32_t kFaithfulTextureCount = 181u;

struct EmbeddedFile {
    const char* data;
    size_t size;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

EmbeddedFile embedded(const char* data, size_t size) {
    return {data, size};
}

json parse_json(EmbeddedFile file) {
    return json::parse(file.data, file.data + file.size);
}

void write_bytes(
    const std::filesystem::path& path,
    const void* data,
    size_t size
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream ||
        (size != 0u &&
         !stream.write(static_cast<const char*>(data),
                       static_cast<std::streamsize>(size)))) {
        throw std::runtime_error("cannot write " + path.string());
    }
    stream.flush();
    if (!stream) throw std::runtime_error("cannot flush " + path.string());
    stream.close();
    if (!stream) throw std::runtime_error("cannot close " + path.string());
}

void write_json(const std::filesystem::path& path, const json& value) {
    const std::string text = value.dump(2) + '\n';
    write_bytes(path, text.data(), text.size());
}

void append_png_bytes(void* context, void* data, int size) {
    auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
    const auto* first = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), first, first + size);
}

std::vector<uint8_t> encode_png(
    const uint8_t* rgba,
    int width,
    int height
) {
    std::vector<uint8_t> bytes;
    if (width <= 0 || height <= 0 ||
        stbi_write_png_to_func(
            append_png_bytes,
            &bytes,
            width,
            height,
            4,
            rgba,
            width * 4
        ) == 0) {
        throw std::runtime_error("PNG encoding failed");
    }
    return bytes;
}

void write_png(
    const std::filesystem::path& path,
    const uint8_t* rgba,
    int width,
    int height
) {
    const std::vector<uint8_t> bytes = encode_png(rgba, width, height);
    write_bytes(path, bytes.data(), bytes.size());
}

Image load_png(EmbeddedFile file) {
    int channels = 0;
    Image image{};
    uint8_t* decoded = stbi_load_from_memory(
        reinterpret_cast<const uint8_t*>(file.data),
        static_cast<int>(file.size),
        &image.width,
        &image.height,
        &channels,
        4
    );
    if (decoded == nullptr || image.width <= 0 || image.height <= 0) {
        stbi_image_free(decoded);
        throw std::runtime_error("embedded PNG decode failed");
    }
    image.rgba.assign(
        decoded,
        decoded + static_cast<size_t>(image.width) * image.height * 4u
    );
    stbi_image_free(decoded);
    return image;
}

Image cover_resize_linearized(
    const Image& source,
    int output_width,
    int output_height
) {
    Image output{
        output_width,
        output_height,
        std::vector<uint8_t>(
            static_cast<size_t>(output_width) * output_height * 4u
        )
    };
    const double source_aspect =
        static_cast<double>(source.width) / source.height;
    const double output_aspect =
        static_cast<double>(output_width) / output_height;
    const double crop_width = source_aspect > output_aspect
        ? source.height * output_aspect
        : source.width;
    const double crop_height = source_aspect > output_aspect
        ? source.height
        : source.width / output_aspect;
    const double crop_x = (source.width - crop_width) * 0.5;
    const double crop_y = (source.height - crop_height) * 0.5;

    std::array<uint8_t, 256> linear{};
    for (size_t index = 0; index < linear.size(); ++index) {
        linear[index] = static_cast<uint8_t>(std::clamp(
            std::lround(
                std::pow(static_cast<double>(index) / 255.0, 2.2) * 255.0
            ),
            0l,
            255l
        ));
    }

    for (int y = 0; y < output_height; ++y) {
        const double sy = crop_y +
            ((static_cast<double>(y) + 0.5) * crop_height / output_height) -
            0.5;
        const int y0 = std::clamp(
            static_cast<int>(std::floor(sy)), 0, source.height - 1
        );
        const int y1 = std::min(y0 + 1, source.height - 1);
        const double fy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
        for (int x = 0; x < output_width; ++x) {
            const double sx = crop_x +
                ((static_cast<double>(x) + 0.5) * crop_width / output_width) -
                0.5;
            const int x0 = std::clamp(
                static_cast<int>(std::floor(sx)), 0, source.width - 1
            );
            const int x1 = std::min(x0 + 1, source.width - 1);
            const double fx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
            const size_t offsets[4]{
                (static_cast<size_t>(y0) * source.width + x0) * 4u,
                (static_cast<size_t>(y0) * source.width + x1) * 4u,
                (static_cast<size_t>(y1) * source.width + x0) * 4u,
                (static_cast<size_t>(y1) * source.width + x1) * 4u,
            };
            const size_t destination =
                (static_cast<size_t>(y) * output_width + x) * 4u;
            for (int channel = 0; channel < 4; ++channel) {
                const double p00 = channel == 3
                    ? source.rgba[offsets[0] + channel]
                    : linear[source.rgba[offsets[0] + channel]];
                const double p10 = channel == 3
                    ? source.rgba[offsets[1] + channel]
                    : linear[source.rgba[offsets[1] + channel]];
                const double p01 = channel == 3
                    ? source.rgba[offsets[2] + channel]
                    : linear[source.rgba[offsets[2] + channel]];
                const double p11 = channel == 3
                    ? source.rgba[offsets[3] + channel]
                    : linear[source.rgba[offsets[3] + channel]];
                const double top = p00 + (p10 - p00) * fx;
                const double bottom = p01 + (p11 - p01) * fx;
                output.rgba[destination + channel] =
                    static_cast<uint8_t>(std::clamp(
                        std::lround(top + (bottom - top) * fy),
                        0l,
                        255l
                    ));
            }
        }
    }
    return output;
}

uint32_t parse_hex_offset(const std::string& value) {
    if (value.size() != 10u || value.substr(0, 2) != "0x") {
        throw std::runtime_error("invalid ROM offset");
    }
    size_t consumed = 0;
    const uint32_t offset = static_cast<uint32_t>(
        std::stoul(value.substr(2), &consumed, 16)
    );
    if (consumed != 8u) {
        throw std::runtime_error("invalid ROM offset");
    }
    return offset;
}

uint8_t expand_five(uint8_t value) {
    return static_cast<uint8_t>((value << 3u) | (value >> 2u));
}

uint8_t expand_four(uint8_t value) {
    return static_cast<uint8_t>((value << 4u) | value);
}

std::vector<uint8_t> decode_texture(
    const uint8_t* source,
    size_t source_size,
    std::string_view format,
    uint32_t width,
    uint32_t height
) {
    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(pixel_count * 4u);
    auto require = [&](size_t bytes) {
        if (source_size < bytes) {
            throw std::runtime_error("ROM texture is truncated");
        }
    };
    if (format == "RGBA16_BE" || format == "IA16_BE") {
        require(pixel_count * 2u);
    } else if (format == "RGBA32") {
        require(pixel_count * 4u);
    } else if (format == "IA8" || format == "I8") {
        require(pixel_count);
    } else if (format == "IA4" || format == "I4") {
        require((pixel_count + 1u) / 2u);
    } else {
        throw std::runtime_error("unsupported ROM texture format");
    }

    for (size_t index = 0; index < pixel_count; ++index) {
        uint8_t r = 0, g = 0, b = 0, a = 0;
        if (format == "RGBA16_BE") {
            const uint16_t value = static_cast<uint16_t>(
                (source[index * 2u] << 8u) | source[index * 2u + 1u]
            );
            r = expand_five((value >> 11u) & 0x1Fu);
            g = expand_five((value >> 6u) & 0x1Fu);
            b = expand_five((value >> 1u) & 0x1Fu);
            a = (value & 1u) != 0u ? 255u : 0u;
        } else if (format == "RGBA32") {
            r = source[index * 4u];
            g = source[index * 4u + 1u];
            b = source[index * 4u + 2u];
            a = source[index * 4u + 3u];
        } else if (format == "IA16_BE") {
            r = g = b = source[index * 2u];
            a = source[index * 2u + 1u];
        } else if (format == "IA8") {
            const uint8_t value = source[index];
            r = g = b = expand_four((value >> 4u) & 0xFu);
            a = expand_four(value & 0xFu);
        } else if (format == "I8") {
            r = g = b = source[index];
            a = source[index];
        } else {
            const uint8_t packed = source[index >> 1u];
            const uint8_t value = (index & 1u) == 0u
                ? static_cast<uint8_t>(packed >> 4u)
                : static_cast<uint8_t>(packed & 0xFu);
            if (format == "IA4") {
                const uint8_t intensity = value & 0xEu;
                r = g = b = static_cast<uint8_t>(
                    (intensity << 4u) | (intensity << 1u) | (intensity >> 2u)
                );
                a = (value & 1u) != 0u ? 255u : 0u;
            } else {
                r = g = b = a = expand_four(value);
            }
        }
        const size_t destination = index * 4u;
        rgba[destination] = r;
        rgba[destination + 1u] = g;
        rgba[destination + 2u] = b;
        rgba[destination + 3u] = a;
    }
    return rgba;
}

std::vector<uint8_t> nearest_scale(
    const std::vector<uint8_t>& source,
    uint32_t width,
    uint32_t height,
    uint32_t scale_log2
) {
    const uint32_t scale = 1u << scale_log2;
    const uint32_t output_width = width * scale;
    const uint32_t output_height = height * scale;
    std::vector<uint8_t> output(
        static_cast<size_t>(output_width) * output_height * 4u
    );
    for (uint32_t y = 0; y < output_height; ++y) {
        for (uint32_t x = 0; x < output_width; ++x) {
            const size_t from =
                (static_cast<size_t>(y >> scale_log2) * width +
                 (x >> scale_log2)) * 4u;
            const size_t to =
                (static_cast<size_t>(y) * output_width + x) * 4u;
            std::copy_n(source.data() + from, 4u, output.data() + to);
        }
    }
    return output;
}

std::vector<uint8_t> scale2x(
    const std::vector<uint8_t>& source,
    uint32_t width,
    uint32_t height
) {
    const uint32_t output_width = width * 2u;
    std::vector<uint8_t> output(
        static_cast<size_t>(output_width) * height * 2u * 4u
    );
    auto pixel = [&](uint32_t x, uint32_t y) {
        return source.data() + (static_cast<size_t>(y) * width + x) * 4u;
    };
    auto equal = [](const uint8_t* left, const uint8_t* right) {
        return std::equal(left, left + 4u, right);
    };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* e = pixel(x, y);
            const uint8_t* b = pixel(x, y > 0u ? y - 1u : y);
            const uint8_t* d = pixel(x > 0u ? x - 1u : x, y);
            const uint8_t* f = pixel(x + 1u < width ? x + 1u : x, y);
            const uint8_t* h = pixel(x, y + 1u < height ? y + 1u : y);
            const uint8_t* values[4]{e, e, e, e};
            if (!equal(b, h) && !equal(d, f)) {
                if (equal(d, b)) values[0] = d;
                if (equal(b, f)) values[1] = f;
                if (equal(d, h)) values[2] = d;
                if (equal(h, f)) values[3] = f;
            }
            const size_t top =
                (static_cast<size_t>(y * 2u) * output_width + x * 2u) * 4u;
            const size_t bottom = top + static_cast<size_t>(output_width) * 4u;
            std::copy_n(values[0], 4u, output.data() + top);
            std::copy_n(values[1], 4u, output.data() + top + 4u);
            std::copy_n(values[2], 4u, output.data() + bottom);
            std::copy_n(values[3], 4u, output.data() + bottom + 4u);
        }
    }
    return output;
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
    bytes.push_back(static_cast<uint8_t>(value >> 16u));
    bytes.push_back(static_cast<uint8_t>(value >> 24u));
}

std::vector<uint8_t> make_rgba8_unorm_dds(
    std::vector<uint8_t> level,
    uint32_t width,
    uint32_t height,
    uint32_t mip_count
) {
    if (width == 0u || height == 0u || mip_count == 0u ||
        level.size() != static_cast<size_t>(width) * height * 4u) {
        throw std::runtime_error("invalid DDS dimensions");
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(148u + level.size() * 4u / 3u);
    append_u32(bytes, 0x20534444u);
    append_u32(bytes, 124u);
    append_u32(bytes, 0x0002100Fu);
    append_u32(bytes, height);
    append_u32(bytes, width);
    append_u32(bytes, width * 4u);
    append_u32(bytes, 0u);
    append_u32(bytes, mip_count);
    for (uint32_t index = 0; index < 11u; ++index) append_u32(bytes, 0u);
    append_u32(bytes, 32u);
    append_u32(bytes, 0x4u);
    append_u32(bytes, 0x30315844u);
    for (uint32_t index = 0; index < 5u; ++index) append_u32(bytes, 0u);
    append_u32(bytes, 0x00401008u);
    for (uint32_t index = 0; index < 4u; ++index) append_u32(bytes, 0u);
    append_u32(bytes, 28u);
    append_u32(bytes, 3u);
    append_u32(bytes, 0u);
    append_u32(bytes, 1u);
    append_u32(bytes, 0u);
    if (bytes.size() != 148u) {
        throw std::runtime_error("DDS header construction failed");
    }

    uint32_t level_width = width;
    uint32_t level_height = height;
    for (uint32_t mip = 0; mip < mip_count; ++mip) {
        bytes.insert(bytes.end(), level.begin(), level.end());
        if (mip + 1u == mip_count) {
            break;
        }
        const uint32_t next_width = std::max(level_width / 2u, 1u);
        const uint32_t next_height = std::max(level_height / 2u, 1u);
        std::vector<uint8_t> next(
            static_cast<size_t>(next_width) * next_height * 4u
        );
        for (uint32_t y = 0; y < next_height; ++y) {
            for (uint32_t x = 0; x < next_width; ++x) {
                const size_t from =
                    (static_cast<size_t>(
                        std::min(y * 2u, level_height - 1u)
                     ) * level_width +
                     std::min(x * 2u, level_width - 1u)) * 4u;
                const size_t to =
                    (static_cast<size_t>(y) * next_width + x) * 4u;
                std::copy_n(level.data() + from, 4u, next.data() + to);
            }
        }
        level = std::move(next);
        level_width = next_width;
        level_height = next_height;
    }
    return bytes;
}

json replacement_database(const json& textures, const char* operation) {
    json records = json::array();
    for (const json& texture : textures) {
        const std::string hash = texture.at("rt64_hash").get<std::string>();
        const std::string path = texture.contains("output")
            ? texture.at("output").get<std::string>()
            : hash + ".png";
        records.push_back({
            {"path", path},
            {"hashes", {{"rt64", hash}, {"rice", ""}}},
            {"operation", operation},
            {"shift", "none"},
            {"preserveOriginalAlpha", texture.value("preserveOriginalAlpha", false)},
        });
    }
    return {
        {"configuration", {
            {"configurationVersion", 3},
            {"autoPath", "rt64"},
            {"defaultOperation", operation},
            {"defaultShift", "none"},
            {"hashVersion", 5},
        }},
        {"textures", records},
        {"operationFilters", json::array()},
        {"shiftFilters", json::array()},
        {"extraFiles", json::array()},
    };
}

void build_menu_assets(
    const std::filesystem::path& root,
    const std::vector<uint8_t>& rom,
    bumble::first_run::PreparationProgress& progress
) {
    const json hashes = parse_json(embedded(
        BumbleMenuHashMapping,
        BumbleMenuHashMapping_size
    ));
    const json ui_mapping = parse_json(embedded(
        BumbleGameplayUiMapping,
        BumbleGameplayUiMapping_size
    ));
    const auto& row_hashes = hashes.at("row_hashes");
    const auto& logo_hashes = hashes.at("suppressed_original_logo_hashes");
    const auto& ui_textures = ui_mapping.at("textures");
    if (row_hashes.size() != kMenuStripCount ||
        logo_hashes.size() != kLogoCount ||
        ui_textures.size() != kUiTextureCount) {
        throw std::runtime_error("menu mapping count changed");
    }

    write_bytes(
        root / "Background_Menus.png",
        BumbleMenuPanelBackground,
        BumbleMenuPanelBackground_size
    );
    write_bytes(
        root / "project_visual_assets.json",
        BumbleProjectVisualAssets,
        BumbleProjectVisualAssets_size
    );
    const Image source = load_png(embedded(
        BumbleMenuBackground,
        BumbleMenuBackground_size
    ));
    const std::array<std::tuple<const char*, int, int>, 2> variants{{
        {"standard_4x3", 1920, 1440},
        {"widescreen_16x9", 2560, 1440},
    }};
    const std::vector<uint8_t> transparent(32u * 32u * 4u, 0u);

    for (const auto& [name, width, height] : variants) {
        progress.check();
        const std::filesystem::path directory = root / name;
        std::filesystem::create_directories(directory);
        const Image background =
            cover_resize_linearized(source, width, height);
        const int strip_height = height / static_cast<int>(kMenuStripCount);
        json database_textures = json::array();

        for (uint32_t row = 0; row < kMenuStripCount; ++row) {
            progress.check();
            const std::string hash = row_hashes.at(row).get<std::string>();
            const std::string filename = hash + ".png";
            const uint8_t* pixels = background.rgba.data() +
                static_cast<size_t>(row) * strip_height * width * 4u;
            write_png(directory / filename, pixels, width, strip_height);
            database_textures.push_back({
                {"rt64_hash", hash},
            });
        }
        for (const json& value : logo_hashes) {
            const std::string hash = value.get<std::string>();
            write_png(
                directory / (hash + ".png"),
                transparent.data(),
                32,
                32
            );
            database_textures.push_back({
                {"rt64_hash", hash},
            });
        }
        for (const json& texture : ui_textures) {
            progress.check();
            const uint32_t offset = parse_hex_offset(
                texture.at("rom_offset").get<std::string>()
            );
            const uint32_t size = texture.at("source_size").get<uint32_t>();
            const uint32_t source_width =
                texture.at("source_width").get<uint32_t>();
            const uint32_t source_height =
                texture.at("source_height").get<uint32_t>();
            if (offset > rom.size() || size > rom.size() - offset) {
                throw std::runtime_error("gameplay UI ROM range changed");
            }
            std::vector<uint8_t> pixels = decode_texture(
                rom.data() + offset,
                size,
                texture.at("source_format").get<std::string>(),
                source_width,
                source_height
            );
            uint32_t scaled_width = source_width;
            uint32_t scaled_height = source_height;
            for (uint32_t pass = 0; pass < 5u; ++pass) {
                pixels = scale2x(pixels, scaled_width, scaled_height);
                scaled_width *= 2u;
                scaled_height *= 2u;
            }
            const std::string hash =
                texture.at("rt64_hash").get<std::string>();
            write_png(
                directory / (hash + ".png"),
                pixels.data(),
                static_cast<int>(scaled_width),
                static_cast<int>(scaled_height)
            );
            database_textures.push_back({
                {"rt64_hash", hash},
            });
        }
        write_json(
            directory / "rt64.json",
            replacement_database(database_textures, "preload")
        );
    }
    write_json(root / "manifest.json", {
        {"schema_version", 2},
        {"generator", std::string(kGenerator)},
        {"source_kind", "user_authored_plus_verified_external_rom"},
        {"rom_included", false},
        {"replacement_count_per_variant", kMenuReplacementCount},
        {"guest_text_owner", "native_embedded_roboto_overlay"},
    });
}

void build_faithful_pack(
    const std::filesystem::path& root,
    const std::vector<uint8_t>& rom,
    bumble::first_run::PreparationProgress& progress,
    bool enhanced = false
) {
    const json mapping = parse_json(embedded(
        BumbleFaithfulTextureMapping,
        BumbleFaithfulTextureMapping_size
    ));
    json textures = mapping.at("textures");
    if (textures.size() != kFaithfulTextureCount) {
        throw std::runtime_error("faithful texture count changed");
    }
    if (enhanced) {
        textures = bumble::textures::enhanced_catalog(rom, std::move(textures));
        const auto menu = parse_json(embedded(BumbleMenuHashMapping, BumbleMenuHashMapping_size));
        std::unordered_set<std::string> reserved;
        for (const auto& hash : menu.at("row_hashes")) reserved.insert(hash.get<std::string>());
        for (const auto& hash : menu.at("suppressed_original_logo_hashes")) reserved.insert(hash.get<std::string>());
        for (auto entry = textures.begin(); entry != textures.end(); ) {
            if (reserved.count(entry->at("rt64_hash").get<std::string>())) entry = textures.erase(entry);
            else ++entry;
        }
    }

    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0u, 2u * 1024u * 1024u)) {
        throw std::runtime_error("cannot create texture archive");
    }
    bool archive_open = true;
    uint32_t completed = 0;
    const char* stage = enhanced ? "Enhancing textures" : "Preparing original textures";
    progress.report(stage, 0, uint32_t(textures.size()));
    try {
        struct Pending { std::string path; std::future<std::vector<uint8_t>> bytes; };
        std::deque<Pending> pending;
        uint32_t reconstructed_source = UINT32_MAX;
        bumble::textures::Image reconstructed_background;
        std::vector<json> backgrounds;
        for (const auto& texture : textures) {
            if (texture.contains("whole_image_offset") && (backgrounds.empty() ||
                    backgrounds.back().at("whole_image_offset") != texture.at("whole_image_offset")))
                backgrounds.push_back(texture);
        }
        size_t next_background = 0;
        std::deque<std::future<bumble::textures::Image>> background_jobs;
        struct CancelOnFailure {
            bumble::first_run::PreparationProgress& progress;
            int exceptions = std::uncaught_exceptions();
            ~CancelOnFailure() {
                if (std::uncaught_exceptions() > exceptions) progress.cancelled.store(true);
            }
        } cancel_on_failure{progress};
        const auto queue_backgrounds = [&]() {
            while (background_jobs.size() < 4 && next_background < backgrounds.size()) {
                const auto description = backgrounds[next_background++];
                background_jobs.push_back(std::async(std::launch::async, [&, description]() {
                    progress.check();
                    const uint32_t offset = description.at("whole_image_offset");
                    const uint32_t width = description.at("source_width");
                    const uint32_t height = description.at("whole_image_height");
                    const auto decoded = decode_texture(rom.data() + offset, rom.size() - offset,
                        description.at("source_format").get<std::string>(), width, height);
                    return bumble::textures::reconstruct_material({width, height, decoded}, [&]() { progress.check(); });
                }));
            }
        };
        const auto write_next = [&]() {
            progress.check();
            const auto bytes = pending.front().bytes.get();
            progress.check();
            if (!mz_zip_writer_add_mem(&archive, pending.front().path.c_str(), bytes.data(),
                    bytes.size(), MZ_BEST_COMPRESSION))
                throw std::runtime_error("cannot add reconstructed texture");
            pending.pop_front();
            progress.report(stage, ++completed, uint32_t(textures.size()));
        };
        for (json& texture : textures) {
            progress.check();
            const uint32_t offset = parse_hex_offset(
                texture.at("rom_offset").get<std::string>()
            );
            const uint32_t size = texture.at("source_size").get<uint32_t>();
            const uint32_t source_width =
                texture.at("source_width").get<uint32_t>();
            const uint32_t source_height =
                texture.at("source_height").get<uint32_t>();
            const uint32_t scale =
                texture.at("nearest_scale_log2").get<uint32_t>();
            const uint32_t output_width =
                texture.at("output_width").get<uint32_t>();
            const uint32_t output_height =
                texture.at("output_height").get<uint32_t>();
            const uint32_t mip_count =
                texture.at("mip_levels").get<uint32_t>();
            if (offset > rom.size() || size > rom.size() - offset ||
                source_width * (1u << scale) != output_width ||
                source_height * (1u << scale) != output_height) {
                throw std::runtime_error("faithful texture mapping changed");
            }
            const auto format = texture.at("source_format").get<std::string>();
            std::vector<uint8_t> pixels = decode_texture(
                rom.data() + offset,
                size,
                format == "I4_IA16_PALETTE" ? "I4" : format,
                source_width,
                source_height
            );
            if (texture.contains("source_rows")) {
                const auto& rows = texture.at("source_rows");
                if (source_width != 1 || rows.size() != source_height || format != "RGBA32")
                    throw std::runtime_error("Composite texture layout changed");
                for (size_t row = 0; row < rows.size(); ++row) {
                    const auto source = rows[row].get<uint32_t>();
                    if (source > rom.size() || rom.size() - source < 4)
                        throw std::runtime_error("Composite texture is truncated");
                    std::copy_n(rom.data() + source, 4, pixels.data() + row * 4);
                }
            }
            if (format == "I4_IA16_PALETTE") {
                const uint32_t palette = texture.at("palette_offset");
                if (palette > rom.size() || rom.size() - palette < 32)
                    throw std::runtime_error("Texture palette is truncated");
                for (size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
                    const uint8_t packed = rom[offset + pixel / 2];
                    const uint32_t index = (pixel & 1) ? (packed & 15) : (packed >> 4);
                    pixels[pixel * 4] = pixels[pixel * 4 + 1] = pixels[pixel * 4 + 2] = rom[palette + index * 2];
                    pixels[pixel * 4 + 3] = rom[palette + index * 2 + 1];
                }
            }
            if (enhanced) {
                bool opaque = true;
                for (size_t alpha = 3; alpha < pixels.size(); alpha += 4)
                    opaque &= pixels[alpha] == 255;
                texture["preserveOriginalAlpha"] = !opaque;
                texture["output_width"] = source_width * 9u;
                texture["output_height"] = source_height * 9u;
                texture.erase("nearest_scale_log2");
                uint32_t levels = 1;
                for (uint32_t w = source_width * 9u, h = source_height * 9u; w > 1 || h > 1; ++levels) {
                    w = std::max(1u, w / 2); h = std::max(1u, h / 2);
                }
                texture["mip_levels"] = levels;
            }
            const std::string output =
                texture.at("output").get<std::string>();
            const bool strip = enhanced && texture.contains("whole_image_offset");
            if (strip) {
                const uint32_t whole = texture.at("whole_image_offset");
                if (whole != reconstructed_source) {
                    progress.report("Enhancing backgrounds", completed, uint32_t(textures.size()));
                    while (!pending.empty()) write_next();
                    queue_backgrounds();
                    reconstructed_background = background_jobs.front().get();
                    background_jobs.pop_front();
                    reconstructed_source = whole;
                    progress.check();
                    queue_backgrounds();
                }
                const uint32_t top = texture.at("view_top").get<uint32_t>() * 9;
                const auto begin = reconstructed_background.rgba.begin() + size_t(top) * source_width * 9 * 4;
                pixels.assign(begin, begin + size_t(source_height) * source_width * 81 * 4);
            }
            pending.push_back({output, std::async(enhanced ? std::launch::async : std::launch::deferred,
                [pixels = std::move(pixels), source_width, source_height, scale, output_width, output_height, mip_count, enhanced, strip, &progress]() {
                    return strip ? bumble::textures::make_dds({source_width * 9, source_height * 9, pixels}) : enhanced
                        ? bumble::textures::make_dds(bumble::textures::reconstruct_material({source_width, source_height, pixels}, [&]() { progress.check(); }))
                        : bumble::first_run::make_faithful_dds(nearest_scale(pixels, source_width, source_height, scale),
                            output_width, output_height, mip_count);
                })});
            if (pending.size() == (enhanced && !strip ? 4u : 1u)) write_next();
        }
        while (!pending.empty()) write_next();

        json database_json = replacement_database(textures, enhanced ? "stream" : "preload");
        if (enhanced) {
            database_json["configuration"]["defaultShift"] = "half";
            for (auto& texture : database_json["textures"]) {
                texture["shift"] = "half";
            }
        }
        const std::string database = database_json.dump(2) + '\n';
        if (!mz_zip_writer_add_mem(
                &archive,
                "rt64.json",
                database.data(),
                database.size(),
                MZ_BEST_COMPRESSION)) {
            throw std::runtime_error("cannot add texture database");
        }
        void* archive_bytes = nullptr;
        size_t archive_size = 0u;
        if (!mz_zip_writer_finalize_heap_archive(
                &archive,
                &archive_bytes,
                &archive_size)) {
            throw std::runtime_error("cannot finalize texture archive");
        }
        const std::unique_ptr<void, decltype(&mz_free)> archive_data(archive_bytes, mz_free);
        write_bytes(root / "textures.rtz", archive_data.get(), archive_size);
        if (!mz_zip_writer_end(&archive)) {
            archive_open = false;
            throw std::runtime_error("cannot close texture archive");
        }
        archive_open = false;
        write_json(root / "manifest.json", {
            {"schema_version", 4},
            {"generator", std::string(kGenerator)},
            {"rom_included", false},
            {"distributable_assets_included", false},
            {"source", "verified_external_us_rev0_rom"},
            {"source_rom_size", kRomSize},
            {"source_rom_sha256", std::string(kRomSha256)},
            {"algorithm", enhanced ? "coordinate-v7" : "nearest_texel_exact"},
            {"output_format", enhanced ? "R8G8B8A8_UNORM_area_mips" : std::string(bumble::first_run::kFaithfulTextureFormat)},
            {"reconstruction_scale", enhanced ? 9u : 0u},
            {"texture_count", textures.size()},
            {"pack_name", "textures.rtz"},
            {"textures", textures},
        });
    } catch (...) {
        if (archive_open) {
            mz_zip_writer_end(&archive);
        }
        throw;
    }
}

bool current_assets(const std::filesystem::path& root) {
    try {
        std::ifstream stream(root / "manifest.json");
        const json manifest = json::parse(stream);
        if (manifest.at("schema_version").get<uint32_t>() != 1u ||
            manifest.at("generator").get<std::string>() != kGenerator ||
            manifest.at("source_rom_sha256").get<std::string>() !=
                kRomSha256) {
            return false;
        }
        for (const char* variant : {"standard_4x3", "widescreen_16x9"}) {
            const std::filesystem::path directory = root / "menu" / variant;
            uint32_t png_count = 0u;
            for (const auto& item :
                 std::filesystem::directory_iterator(directory)) {
                if (item.is_regular_file() &&
                    item.path().extension() == ".png") {
                    ++png_count;
                }
            }
            if (png_count != kMenuReplacementCount ||
                !std::filesystem::is_regular_file(directory / "rt64.json")) {
                return false;
            }
        }
        return
            std::filesystem::is_regular_file(
                root / "menu" / "Background_Menus.png") &&
            std::filesystem::is_regular_file(
                root / "death" / "DeathScreen.png") &&
            std::filesystem::is_regular_file(
                root / "completion" /
                "BuckBumble_GameCompletion_Background.png") &&
            std::filesystem::is_regular_file(
                root / "textures" / "textures.rtz");
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> read_rom(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (!stream || size != static_cast<std::streamoff>(kRomSize)) {
        throw std::runtime_error("verified ROM cannot be reopened");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("verified ROM cannot be read");
    }
    return bytes;
}

void build_assets(
    const std::filesystem::path& root,
    const std::filesystem::path& rom_path,
    bumble::first_run::PreparationProgress& progress,
    bool enhanced
) {
    const std::vector<uint8_t> rom = read_rom(rom_path);
    std::filesystem::create_directories(root);
    build_menu_assets(root / "menu", rom, progress);
    build_faithful_pack(root / "textures", rom, progress);
    if (enhanced) build_faithful_pack(root / "textures-enhanced", rom, progress, true);
    progress.report("Saving textures");

    const EmbeddedFile manifest = embedded(
        BumbleProjectVisualAssets,
        BumbleProjectVisualAssets_size
    );
    write_bytes(
        root / "death" / "manifest.json",
        manifest.data,
        manifest.size
    );
    write_bytes(
        root / "death" / "DeathScreen.png",
        BumbleDeathScreen,
        BumbleDeathScreen_size
    );
    write_bytes(
        root / "completion" / "manifest.json",
        manifest.data,
        manifest.size
    );
    write_bytes(
        root / "completion" /
            "BuckBumble_GameCompletion_Background.png",
        BumbleCompletionBackground,
        BumbleCompletionBackground_size
    );
    write_json(root / "manifest.json", {
        {"schema_version", 1},
        {"generator", std::string(kGenerator)},
        {"source_rom_size", kRomSize},
        {"source_rom_sha256", std::string(kRomSha256)},
        {"rom_included", false},
        {"derived_assets_local_only", true},
        {"menu_replacements_per_variant", kMenuReplacementCount},
        {"faithful_textures", kFaithfulTextureCount},
    });
}

} // namespace

std::vector<uint8_t> bumble::first_run::make_faithful_dds(
    std::vector<uint8_t> pixels, uint32_t width, uint32_t height, uint32_t mip_count
) {
    return make_rgba8_unorm_dds(std::move(pixels), width, height, mip_count);
}

namespace {
void recover_prepared(const std::filesystem::path& target) {
    const std::filesystem::path backup = target.string() + ".previous";
    if (!std::filesystem::exists(target) && std::filesystem::exists(backup))
        std::filesystem::rename(backup, target);
}

void publish_prepared(const std::filesystem::path& staging, const std::filesystem::path& target) {
    const std::filesystem::path backup = target.string() + ".previous";
    std::filesystem::remove_all(backup);
    if (std::filesystem::exists(target)) std::filesystem::rename(target, backup);
    try { std::filesystem::rename(staging, target); }
    catch (...) {
        if (std::filesystem::exists(backup) && !std::filesystem::exists(target))
            std::filesystem::rename(backup, target);
        throw;
    }
    std::error_code error;
    std::filesystem::remove_all(backup, error);
}

bool current_enhanced(const std::filesystem::path& root) {
    try {
        std::ifstream stream(root / "manifest.json");
        const auto manifest = json::parse(stream);
        return manifest.at("generator").get<std::string>() == kGenerator &&
            manifest.at("schema_version").get<unsigned>() == 4 &&
            manifest.at("source_rom_sha256").get<std::string>() == kRomSha256 &&
            std::filesystem::is_regular_file(root / "textures.rtz");
    } catch (...) { return false; }
}
}

bool bumble::first_run::enhanced_textures_available() {
    return g_enhanced_available.load(std::memory_order_acquire);
}

bool bumble::first_run::texture_prompt_requested() {
    return g_texture_prompt_requested.load(std::memory_order_acquire);
}

bool bumble::first_run::request_texture_prompt() {
    try {
        if (g_texture_preference.empty()) return false;
        const std::filesystem::path staging = g_texture_preference.string() + ".building";
        write_json(staging, {{"schema_version", 1}, {"choice", "ask"}});
        publish_prepared(staging, g_texture_preference);
        g_texture_prompt_requested.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}

bumble::first_run::AssetResult bumble::first_run::ensure_assets(
    const std::filesystem::path& data_root,
    const std::filesystem::path& rom_path,
    bool unattended
) {
    g_enhanced_available.store(false, std::memory_order_release);
    const auto assets = data_root / "assets";
    const auto enhanced = assets / "textures-enhanced";
    const auto preference = data_root / "config" / "texture-preparation.json";
    g_texture_preference = preference;
    g_texture_prompt_requested.store(false, std::memory_order_release);
    std::filesystem::path staging;
    try {
        recover_prepared(assets);
        recover_prepared(enhanced);
        recover_prepared(preference);
        const bool base_ready = current_assets(assets);
        const bool enhanced_ready = base_ready && current_enhanced(enhanced);
        std::string policy = "ask";
        if (!unattended) {
            try {
                std::ifstream stream(preference);
                const auto saved = json::parse(stream);
                if (saved.at("schema_version").get<unsigned>() == 1)
                    policy = saved.at("choice").get<std::string>();
            } catch (...) {}
        }
        TextureChoice choice;
        choice.generate = unattended || policy == "generate" || (enhanced_ready && policy != "skip");
        if (!unattended && !enhanced_ready && policy != "generate" && policy != "skip")
            choice = choose_enhanced_textures();

        const bool prepare_base = !base_ready;
        const bool prepare_enhanced = choice.generate && !enhanced_ready;
        if (prepare_base || prepare_enhanced) {
            const auto target = prepare_base ? assets : enhanced;
            staging = target.string() + ".building";
            std::filesystem::remove_all(staging);
            run_preparation([&](PreparationProgress& progress) {
                if (prepare_base) build_assets(staging, rom_path, progress, choice.generate);
                else build_faithful_pack(staging, read_rom(rom_path), progress, true);
                progress.check();
                publish_prepared(staging, target);
            });
        }
        if (choice.remember && !unattended) {
            const std::filesystem::path temporary = preference.string() + ".building";
            write_json(temporary, {{"schema_version", 1}, {"choice", choice.generate ? "generate" : "skip"}});
            publish_prepared(temporary, preference);
        }
        g_enhanced_available.store(choice.generate, std::memory_order_release);
        std::fprintf(stderr, "BUMBLE_FIRST_RUN stage=assets_ready source=%s enhanced=%d generator=%.*s\n",
            prepare_base || prepare_enhanced ? "generated" : "existing", choice.generate ? 1 : 0,
            static_cast<int>(kGenerator.size()), kGenerator.data());
        std::fflush(stderr);
        return AssetResult::Ready;
    } catch (const PreparationCancelled&) {
        std::error_code error;
        if (!staging.empty()) std::filesystem::remove_all(staging, error);
        return AssetResult::Cancelled;
    } catch (const std::exception& exception) {
        std::error_code error;
        if (!staging.empty()) std::filesystem::remove_all(staging, error);
        std::fprintf(stderr, "BUMBLE_FIRST_RUN stage=assets_failed reason=%s\n", exception.what());
        std::fflush(stderr);
        return AssetResult::Failed;
    }
}
