#include "native_first_run_assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
            a = 255u;
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
    const std::vector<uint8_t>& rom
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
        const std::filesystem::path directory = root / name;
        std::filesystem::create_directories(directory);
        const Image background =
            cover_resize_linearized(source, width, height);
        const int strip_height = height / static_cast<int>(kMenuStripCount);
        json database_textures = json::array();

        for (uint32_t row = 0; row < kMenuStripCount; ++row) {
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
    const std::vector<uint8_t>& rom
) {
    const json mapping = parse_json(embedded(
        BumbleFaithfulTextureMapping,
        BumbleFaithfulTextureMapping_size
    ));
    const json& textures = mapping.at("textures");
    if (textures.size() != kFaithfulTextureCount) {
        throw std::runtime_error("faithful texture count changed");
    }

    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0u, 2u * 1024u * 1024u)) {
        throw std::runtime_error("cannot create texture archive");
    }
    bool archive_open = true;
    try {
        for (const json& texture : textures) {
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
            std::vector<uint8_t> pixels = decode_texture(
                rom.data() + offset,
                size,
                texture.at("source_format").get<std::string>(),
                source_width,
                source_height
            );
            pixels = nearest_scale(
                pixels,
                source_width,
                source_height,
                scale
            );
            const std::vector<uint8_t> dds = bumble::first_run::make_faithful_dds(
                std::move(pixels),
                output_width,
                output_height,
                mip_count
            );
            const std::string output =
                texture.at("output").get<std::string>();
            if (!mz_zip_writer_add_mem(
                    &archive,
                    output.c_str(),
                    dds.data(),
                    dds.size(),
                    MZ_BEST_COMPRESSION)) {
                throw std::runtime_error("cannot add faithful texture");
            }
        }

        const std::string database =
            replacement_database(textures, "preload").dump(2) + '\n';
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
        write_bytes(root / "textures.rtz", archive_bytes, archive_size);
        mz_free(archive_bytes);
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
            {"algorithm", "nearest_texel_exact"},
            {"output_format", std::string(bumble::first_run::kFaithfulTextureFormat)},
            {"texture_count", kFaithfulTextureCount},
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
    const std::filesystem::path& rom_path
) {
    const std::vector<uint8_t> rom = read_rom(rom_path);
    std::filesystem::create_directories(root);
    build_menu_assets(root / "menu", rom);
    build_faithful_pack(root / "textures", rom);

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

bool bumble::first_run::ensure_assets(
    const std::filesystem::path& data_root,
    const std::filesystem::path& rom_path
) {
    const std::filesystem::path assets = data_root / "assets";
    if (current_assets(assets)) {
        std::fprintf(
            stderr,
            "BUMBLE_FIRST_RUN stage=assets_ready source=existing"
            " generator=%.*s\n",
            static_cast<int>(kGenerator.size()),
            kGenerator.data()
        );
        return true;
    }

    const std::filesystem::path staging = data_root / "assets.building";
    const std::filesystem::path backup = data_root / "assets.previous";
    try {
        std::error_code error;
        std::filesystem::remove_all(staging, error);
        error.clear();
        std::filesystem::remove_all(backup, error);
        std::fprintf(
            stderr,
            "BUMBLE_FIRST_RUN stage=assets_build_started path=%s\n",
            assets.string().c_str()
        );
        std::fflush(stderr);
        build_assets(staging, rom_path);
        if (std::filesystem::exists(assets)) {
            std::filesystem::rename(assets, backup);
        }
        try {
            std::filesystem::rename(staging, assets);
        } catch (...) {
            if (std::filesystem::exists(backup) &&
                !std::filesystem::exists(assets)) {
                std::filesystem::rename(backup, assets);
            }
            throw;
        }
        std::filesystem::remove_all(backup, error);
        std::fprintf(
            stderr,
            "BUMBLE_FIRST_RUN stage=assets_ready source=generated"
            " generator=%.*s menu_per_variant=%u textures=%u\n",
            static_cast<int>(kGenerator.size()),
            kGenerator.data(),
            kMenuReplacementCount,
            kFaithfulTextureCount
        );
        std::fflush(stderr);
        return true;
    } catch (const std::exception& exception) {
        std::error_code error;
        std::filesystem::remove_all(staging, error);
        std::fprintf(
            stderr,
            "BUMBLE_FIRST_RUN stage=assets_failed reason=%s\n",
            exception.what()
        );
        std::fflush(stderr);
        return false;
    }
}
