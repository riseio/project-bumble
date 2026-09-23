#include "native_texture_catalog.hpp"

#include <array>
#include <cstdio>
#include <set>
#include <stdexcept>
#include "hle/rt64_rdp.h"
#include "xxHash/xxh3.h"
#include "common/rt64_tmem_hasher.h"

namespace bumble::textures {
namespace {
class Catalog {
    const std::vector<uint8_t>& rom;
    std::set<std::string> hashes;
    nlohmann::json verified;
public:
    nlohmann::json textures;
    Catalog(const std::vector<uint8_t>& source, nlohmann::json existing)
        : rom(source), verified(existing), textures(std::move(existing)) {
        for (const auto& texture : textures) hashes.insert(texture.at("rt64_hash").get<std::string>());
    }
    uint32_t word(uint32_t offset) const {
        if (offset > rom.size() || rom.size() - offset < 4) throw std::runtime_error("Texture table is truncated");
        return uint32_t(rom[offset]) << 24 | uint32_t(rom[offset + 1]) << 16 |
            uint32_t(rom[offset + 2]) << 8 | rom[offset + 3];
    }
    uint32_t address(uint32_t value) const {
        if (value >= 0x03000000 && value < 0x0304F400) return 0x539FF0 + value - 0x03000000;
        if (value >= 0x04000000 && value < 0x0404CA88) return 0x61A3F0 + value - 0x04000000;
        if (value >= 0x803D6000 && value < 0x803FD950) return 0x9FDD60 + value - 0x803D6000;
        if (value >= 0x80043970 && value < 0x80100000) return 0x205B0 + value - 0x80043970;
        throw std::runtime_error("Texture address has no ROM owner");
    }
    void add(uint32_t offset, uint16_t width, uint16_t height, uint8_t fmt = 0,
            uint8_t siz = 2, uint32_t palette = 0, uint32_t wholeImage = 0, uint32_t top = 0) {
        const uint32_t size = (uint32_t(width) * height << siz) / 2;
        if (size > 4096 || offset > rom.size() || size > rom.size() - offset ||
            (palette && (palette > rom.size() || rom.size() - palette < 32)))
            throw std::runtime_error("Texture exceeds its source bounds");
        const bool rgba32 = fmt == 0 && siz == 3;
        const uint32_t rowBytes = (uint32_t(width) << (rgba32 ? 2 : siz)) / 2;
        if (rowBytes % 8) throw std::runtime_error("Texture row is not word aligned");
        std::array<uint8_t, 4096> tmem{};
        const uint32_t wordsPerRow = rowBytes * (rgba32 ? 2 : 1) / 8;
        const uint32_t dxt = (2048 + wordsPerRow - 1) / wordsPerRow;
        uint32_t accumulator = 0, swap = 0;
        for (uint32_t word = 0; word < size / 8; ++word) {
            for (uint32_t byte = 0; byte < (rgba32 ? 4u : 8u); ++byte) {
                const uint32_t target = (word * (rgba32 ? 4 : 8) + byte) ^ swap;
                if (rgba32) {
                    const uint32_t source = offset + word * 8 + (byte / 2) * 4 + (byte & 1);
                    tmem.at(target) = rom[source];
                    tmem.at(target + 2048) = rom[source + 2];
                } else tmem.at(target) = rom[offset + word * 8 + byte];
            }
            accumulator += dxt;
            while (accumulator >= 2048) { accumulator -= 2048; swap ^= 4; }
        }
        if (palette) {
            for (uint32_t i = 0; i < 16; ++i)
                for (uint32_t repeat = 0; repeat < 4; ++repeat) {
                    tmem[2048 + i * 8 + repeat * 2] = rom[palette + i * 2];
                    tmem[2049 + i * 8 + repeat * 2] = rom[palette + i * 2 + 1];
                }
        }
        RT64::LoadTile tile{};
        tile.fmt = fmt; tile.siz = siz; tile.line = uint16_t(rowBytes / 8);
        const auto hash = RT64::TMEMHasher::hash(tmem.data(), tile, width, height,
            palette ? 3 : 0, RT64::TMEMHasher::CurrentHashVersion);
        char name[17], location[11];
        std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
        std::snprintf(location, sizeof(location), "0x%08X", offset);
        if (fmt == 0 && siz == 2 && width == 32 && height == 32 && !palette) {
            for (const auto& old : verified)
                if (old.at("rom_offset") == location && old.at("source_width") == width &&
                    old.at("source_height") == height && old.at("source_format") == "RGBA16_BE" &&
                    old.at("rt64_hash") != name)
                    throw std::runtime_error("Texture hash disagrees with the verified mapping");
        }
        if (!hashes.insert(name).second) return;
        const char* format = palette ? "I4_IA16_PALETTE" :
            (fmt == 4 ? (siz == 1 ? "I8" : "I4") : fmt == 3 ?
                (siz == 2 ? "IA16_BE" : "IA4") : (rgba32 ? "RGBA32" : "RGBA16_BE"));
        nlohmann::json entry = {
            {"rt64_hash", name}, {"rom_offset", location}, {"source_size", size},
            {"source_width", width}, {"source_height", height}, {"source_format", format},
            {"nearest_scale_log2", 0}, {"output_width", width}, {"output_height", height},
            {"mip_levels", 1}, {"output", std::string(name) + ".v5.enhanced.dds"}
        };
        if (palette) entry["palette_offset"] = palette;
        if (wholeImage) {
            entry["whole_image_offset"] = wholeImage;
            entry["whole_image_height"] = 240;
            entry["view_top"] = top;
        }
        textures.push_back(std::move(entry));
    }
    void display_list(uint32_t root) {
        uint32_t image = 0, render = 0;
        const auto start = address(root);
        for (uint32_t p = start; p < 0x666E78; p += 8) {
            const auto command = word(p), argument = word(p + 4);
            switch (command >> 24) {
            case 0xDF: return;
            case 0xDE: throw std::runtime_error("Unexpected nested model display list");
            case 0xFD: image = argument; break;
            case 0xF5: if ((argument >> 24) == 0) render = command; break;
            case 0xF2:
                if (image && render && (argument >> 24) == 0 && (command & 0xFFFFFF) == 0) {
                    const uint16_t width = uint16_t(((argument >> 12) & 0xFFF) / 4 + 1);
                    const uint16_t height = uint16_t((argument & 0xFFF) / 4 + 1);
                    if ((render & 0xFFFFFE00) != 0xF5101000 || width != 32 || height != 32)
                        throw std::runtime_error("Model texture layout changed");
                    add(address(image), width, height);
                    image = 0;
                }
                break;
            }
        }
        throw std::runtime_error("Model display list is unterminated");
    }
    void dialog_fill() {
        // The narrow load retains two texels from the preceding corner image.
        const std::array<uint32_t, 3> rows{address(0x800F6F38), address(0x800F6B48), address(0x800F6B50)};
        std::array<uint8_t, 4096> tmem{};
        for (uint32_t y = 0; y < rows.size(); ++y) {
            const auto target = y * 8 ^ ((y & 1) ? 4 : 0);
            tmem[target] = rom.at(rows[y]); tmem[target + 1] = rom.at(rows[y] + 1);
            tmem[target + 2048] = rom.at(rows[y] + 2); tmem[target + 2049] = rom.at(rows[y] + 3);
        }
        RT64::LoadTile tile{};
        tile.siz = 3; tile.line = 1;
        const auto hash = RT64::TMEMHasher::hash(tmem.data(), tile, 1, 3, 0, RT64::TMEMHasher::CurrentHashVersion);
        char name[17], location[11];
        std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
        std::snprintf(location, sizeof(location), "0x%08X", rows[0]);
        if (!hashes.insert(name).second) return;
        textures.push_back({{"rt64_hash", name}, {"rom_offset", location}, {"source_size", 12},
            {"source_width", 1}, {"source_height", 3}, {"source_format", "RGBA32"}, {"source_rows", rows},
            {"nearest_scale_log2", 0}, {"output_width", 1}, {"output_height", 3}, {"mip_levels", 1},
            {"output", std::string(name) + ".v5.enhanced.dds"}});
    }
};
}

nlohmann::json enhanced_catalog(const std::vector<uint8_t>& rom, nlohmann::json textures) {
    Catalog catalog(rom, std::move(textures));
    std::set<uint32_t> materials;
    for (uint32_t i = 0; i < 0x63C; ++i) {
        uint32_t record = 0x666E80 + catalog.word(0xAFC74 + i * 4);
        uint32_t vertices = catalog.word(record + 4);
        if (vertices > (rom.size() - record - 8) / 16) throw std::runtime_error("Material vertices are truncated");
        record += 8 + vertices * 16;
        while (vertices) {
            const auto group = catalog.word(record);
            const uint32_t count = group >> 24, triangles = (group >> 16) & 255, type = (group >> 8) & 255;
            if (!count || count > vertices) throw std::runtime_error("Material group is invalid");
            if (type == 5 || type == 6 || type == 7) materials.insert(catalog.word(record + 4));
            else if (type != 0) throw std::runtime_error("Material format changed");
            vertices -= count;
            record += 8 + ((triangles * 3 + 7) & ~7u);
        }
    }
    for (uint32_t material : materials) {
        catalog.add(material < 0x123 ? 0x5893F0 + catalog.word(0xB1564 + material * 8) :
            catalog.address(material), 32, 32);
    }
    constexpr uint32_t models[] = {
        0x800CE220,0x800D1934,0x800D291C,0x803FC8D0,0x803EC958,0x803EF148,
        0x803E6844,0x803E92A8,0x803E9D68,0x803EB248,0x800CCE30,0x800CCFA8,
        0x800CD120,0x803D690C,0x803D6178,0x803D6040,0x803D8428,0x803D6CF0,
        0x803D7458,0x803D74B8,0x803D81C8,0x803DA468,0x803DD7B8,0x803DEAD8,
        0x803DFEE8,0x803E3570
    };
    std::set<uint32_t> roots;
    for (uint32_t model : models) {
        const auto descriptor = catalog.address(model);
        const auto parts = catalog.word(descriptor) >> 24;
        const auto table = catalog.address(catalog.word(descriptor + 4));
        for (uint32_t part = 0; part < parts; ++part) roots.insert(catalog.word(table + part * 4));
    }
    constexpr uint32_t staticRoots[] = {
        0x04029790,0x04009E30,0x04004230,0x04008EA0,0x04022D18,0x04025DC8,
        0x04023B10,0x04025FF8,0x040242F0,0x040255E8,0x04023E28,0x04029D10,
        0x04022298,0x04022360,0x04024C10,0x04034288,0x04034910,0x04024120,
        0x040399F0,0x04039720,0x040397E0,0x040398A0,0x04018438,0x04017DE8,
        0x0402A9E0,0x04012EA0,0x04023770,0x04010F38,0x040260A0,0x040411C0,
        0x04040DF0,0x04024E28,0x0402BE48,0x0402BCB8,0x0402BB28,0x04037390,
        0x04037078,0x0402A298,0x04023478,0x0400FC00,0x0403EA60,0x0403E8F0,
        0x0403D7F8,0x0403DDC0,0x0403DEB0,0x0403FDA0,0x0403FB00,0x0403FC50,
        0x0403AFC8,0x0403B980,0x0403BA30,0x040422A0,0x04042BF8,0x04042CA8,
        0x04045CD0
    };
    roots.insert(std::begin(staticRoots), std::end(staticRoots));
    for (uint32_t weapon = 0; weapon < 11; ++weapon)
        roots.insert(catalog.word(catalog.address(0x800CD244) + weapon * 24 + 16));
    for (uint32_t root : roots) catalog.display_list(root);
    const auto frames = [&](uint32_t table, uint32_t count, uint16_t side, uint32_t palette) {
        for (uint32_t frame = 0; frame < count; ++frame)
            catalog.add(catalog.address(catalog.word(catalog.address(table) + frame * 4)), side, side, 4, 0, palette);
    };
    const auto palette = catalog.address(0x800D6BD0);
    frames(0x800D6C50, 10, 64, palette);
    frames(0x800D6C78, 10, 64, palette);
    frames(0x800D6CC8, 20, 64, palette);
    for (uint32_t age = 0; age < 40; ++age)
        catalog.add(catalog.address(catalog.word(catalog.address(0x800D6CA0) + (age / 4) * 4)),
            64, 64, 4, 0, palette + (age / 10) * 32);
    frames(0x800D6D18, 8, 64, 0);
    frames(0x800D6D38, 6, 32, 0);
    for (uint32_t frame = 0; frame < 9; ++frame)
        catalog.add(catalog.address(catalog.word(catalog.address(0x800D6DD8) + frame * 4)), 64, 64, 4, 0,
            catalog.address(0x800D6D58) + rom.at(catalog.address(0x800D6E0C) + frame) * 32);
    for (uint32_t digit = 0; digit < 10; ++digit)
        catalog.add(catalog.address(catalog.word(catalog.address(0x800F5A90) + digit * 4)), 32, 32);
    catalog.add(catalog.address(0x800F6F40), 32, 32, 0, 3);
    for (uint32_t icon : {0x800F5F30u, 0x800F6330u, 0x800F6730u, 0x800F6B30u})
        catalog.add(catalog.address(icon), 16, 16, 0, 3);
    catalog.dialog_fill();
    for (uint32_t glyph = 0; glyph < 32; ++glyph) {
        if (glyph != 29)
            catalog.add(0x7FF4C0 + catalog.word(catalog.address(0x800FE668) + glyph * 8), 32, 32, 3, 2);
    }
    catalog.add(catalog.address(0x03028000), 64, 64, 4, 0);
    catalog.add(catalog.address(0x03013000), 64, 128, 4, 0);
    for (uint32_t atlas = 1; atlas < 4; ++atlas)
        catalog.add(catalog.address(catalog.word(catalog.address(0x800FECD0) + atlas * 4)), 64, 128, 3, 0);
    for (uint32_t header : {0x800FC628u, 0x800FC6D0u, 0x800FC778u, 0x800FD85Cu}) {
        const uint32_t source = 0x7FF4C0 + catalog.word(catalog.address(header) + 0x14);
        for (uint32_t y = 0; y < 240; y += 2)
            catalog.add(source + y * 640, 320, 2, 0, 2, 0, source, y);
    }
    for (uint32_t background = 0; background < 10; ++background) {
        const uint32_t source = 0x7FF4C0 + catalog.word(catalog.address(0x800FBFF8) + background * 4);
        for (uint32_t y = 0; y < 240; y += 2)
            catalog.add(source + y * 320, 320, 2, 4, 1, 0, source, y);
    }
    return std::move(catalog.textures);
}
}
