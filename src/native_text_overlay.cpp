#include "native_text_overlay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <json/json.hpp>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include "bumble_menu_background.h"
#include "bumble_text_font.h"
#include "native_graphics_options.hpp"
#include "native_game_completion_screen.hpp"
#include "native_text_overlay_state.hpp"
#include "native_weapon_system.hpp"

#include "common/rt64_plume.h"
#include "common/rt64_bumble_ui.h"
#include "render/rt64_descriptor_sets.h"
#include "render/rt64_shader_library.h"
#include "render/rt64_texture_cache.h"

#include "native_bumble_text_overlay_ps.dxil.h"
#include "native_bumble_text_overlay_ps.spirv.h"
#include "native_bumble_text_overlay_vs.dxil.h"
#include "native_bumble_text_overlay_vs.spirv.h"

namespace {

using namespace plume;

constexpr float kGuestHeight = 240.0f;
constexpr float kGuestWidth = 320.0f;
constexpr float kBriefingFontEmPixelsPerGuestPixel = 7.5f;
constexpr float kBriefingOutlinePixelsPerGuestPixel = 0.36f;
constexpr float kMenuOutlinePixelsPerGuestPixel = 0.36f;
constexpr float kCreditsOutlinePixelsPerGuestPixel = 0.62f;
constexpr float kGameplayFontEmPixelsPerGuestPixel = 14.0f;
constexpr float kGameplayOutlinePixelsPerGuestPixel = 0.8f;
constexpr float kWidePanelMaximumFontScale = 1.00f;
constexpr float kWidePanelMinimumMarginGuestPixels = 24.0f;
constexpr float kBriefingPanelAuthoredWidth = 250.0f;
constexpr float kBriefingPanelTextInset = 4.0f;
constexpr uint32_t kRasterPadding = RT64::BumbleUI::RasterPadding;
constexpr uint32_t kMenuBackgroundWidth = 1672u;
constexpr uint32_t kMenuBackgroundHeight = 941u;
constexpr char kMenuBackgroundHash[] =
    "5d2b1784c93f0bdf7dd00f68e8e5a034cf12f5c37bc4e7fd47165e2382494812";
std::atomic_uint32_t g_last_draw_selected_weapon{UINT32_MAX};

struct RasterizedText {
    struct RevealBoundary {
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
    };
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<RevealBoundary> reveal;
};

struct RenderedSlot {
    bumble::text_overlay::Observation observation;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    RasterizedText pixels;
    RT64::Texture texture;
    std::unique_ptr<RenderBuffer> upload_buffer;
    std::unique_ptr<RT64::VideoInterfaceDescriptorSet> descriptor_set;
    uint64_t last_submission_id = 0;
};

struct RetiredRenderedResources {
    RT64::Texture texture;
    std::unique_ptr<RenderBuffer> upload_buffer;
    std::unique_ptr<RT64::VideoInterfaceDescriptorSet> descriptor_set;
    uint64_t last_submission_id = 0;
};

struct TextOverlayConstants {
    float opacity = 1.0f;
    float padding[3]{};
};

struct OverlayState {
    std::mutex mutex;
    RenderInterface* render_interface = nullptr;
    RenderDevice* device = nullptr;
    const RT64::ShaderLibrary* shader_library = nullptr;
    RenderSwapChain* swap_chain = nullptr;
    uint32_t output_width = 0u;
    uint32_t output_height = 0u;
    std::unique_ptr<RenderPipelineLayout> pipeline_layout;
    std::unique_ptr<RenderPipeline> pipeline;
    std::vector<RenderedSlot> slots;
    std::vector<RetiredRenderedResources> retired_resources;
    uint64_t completed_submission_id = 0;
    std::vector<uint8_t> menu_background_bytes;
    std::vector<uint8_t> main_menu_background_bytes;
    std::unique_ptr<RT64::Texture> menu_background_texture;
    std::unique_ptr<RT64::Texture> main_menu_background_texture;
    std::unique_ptr<RenderBuffer> menu_background_upload_buffer;
    std::unique_ptr<RenderBuffer> main_menu_background_upload_buffer;
    std::unique_ptr<RT64::VideoInterfaceDescriptorSet>
        menu_background_descriptor_set;
    std::unique_ptr<RT64::VideoInterfaceDescriptorSet>
        main_menu_background_descriptor_set;
    stbtt_fontinfo font{};
    bool font_ready = false;
    bool menu_background_configured = false;
    bool menu_background_upload_failure_logged = false;
    bool main_menu_background_upload_failure_logged = false;
    bool menu_background_presented_logged = false;
    bool main_menu_background_presented_logged = false;
    bool wide_panel_layout_logged = false;
    bool editor_presented_logged = false;
    bool ready = false;
    bool presented_logged = false;
};

OverlayState g_state;

void retire_slot_resources(RenderedSlot& slot) {
    const bool owns_gpu_resources = slot.texture.texture != nullptr ||
        slot.upload_buffer != nullptr || slot.descriptor_set != nullptr;
    if (owns_gpu_resources &&
        slot.last_submission_id > g_state.completed_submission_id) {
        RetiredRenderedResources retired;
        retired.texture = std::move(slot.texture);
        retired.upload_buffer = std::move(slot.upload_buffer);
        retired.descriptor_set = std::move(slot.descriptor_set);
        retired.last_submission_id = slot.last_submission_id;
        g_state.retired_resources.emplace_back(std::move(retired));
    }
    else {
        slot.texture = {};
        slot.upload_buffer.reset();
        slot.descriptor_set.reset();
    }
    slot.last_submission_id = 0;
}

bool load_menu_background_asset(const std::filesystem::path& directory) {
    const std::filesystem::path manifest_path =
        directory / "project_visual_assets.json";
    const std::filesystem::path image_path =
        directory / "Background_Menus.png";
    std::ifstream manifest_stream(manifest_path);
    if (!manifest_stream) {
        return false;
    }

    nlohmann::json manifest;
    try {
        manifest_stream >> manifest;
        if (manifest.at("schema_version").get<int>() != 1 ||
            manifest.at("origin").get<std::string>() != "user_authored" ||
            !manifest.at("redistribution_authorized").get<bool>() ||
            manifest.at("contains_rom_bytes").get<bool>() ||
            manifest.at("contains_rom_derived_assets").get<bool>()) {
            return false;
        }
        const auto& assets = manifest.at("assets");
        const auto matching = std::find_if(
            assets.begin(),
            assets.end(),
            [](const nlohmann::json& asset) {
                return asset.at("id").get<std::string>() ==
                    "menu_panel_background";
            }
        );
        if (matching == assets.end() ||
            matching->at("path").get<std::string>() !=
                "NewImages/Background_Menus.png" ||
            matching->at("width").get<uint32_t>() != kMenuBackgroundWidth ||
            matching->at("height").get<uint32_t>() != kMenuBackgroundHeight ||
            matching->at("sha256").get<std::string>() !=
                kMenuBackgroundHash) {
            return false;
        }
    }
    catch (const std::exception&) {
        return false;
    }

    std::ifstream image_stream(image_path, std::ios::binary);
    if (!image_stream) {
        return false;
    }
    image_stream.seekg(0, std::ios::end);
    const std::streamoff size = image_stream.tellg();
    image_stream.seekg(0, std::ios::beg);
    if (size < 8) {
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    image_stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!image_stream ||
        bytes[0] != 0x89u || bytes[1] != 0x50u || bytes[2] != 0x4Eu ||
        bytes[3] != 0x47u || bytes[4] != 0x0Du || bytes[5] != 0x0Au ||
        bytes[6] != 0x1Au || bytes[7] != 0x0Au) {
        return false;
    }
    g_state.menu_background_bytes = std::move(bytes);
    return true;
}

float wide_guest_width(uint32_t output_width, float guest_scale) {
    return guest_scale > 0.0f
        ? static_cast<float>(output_width) / guest_scale
        : kGuestWidth;
}

float wide_panel_margin(float width) {
    if (width <= kGuestWidth + 0.01f) {
        return 0.0f;
    }
    return std::min(
        kWidePanelMinimumMarginGuestPixels,
        std::max(0.0f, width * 0.10f)
    );
}

float wide_panel_font_scale(float width) {
    if (width <= kGuestWidth + 0.01f) {
        return 1.0f;
    }
    return std::clamp(
        width / kGuestWidth,
        1.0f,
        kWidePanelMaximumFontScale
    );
}

bool rasterize_text(
    const bumble::text_overlay::Observation& observation,
    uint32_t output_width,
    uint32_t output_height,
    RasterizedText& result,
    bool measure_only = false
) {
    result = {};
    if (observation.text.empty() || output_height == 0u ||
        !g_state.font_ready) {
        return false;
    }

    const float scale = static_cast<float>(output_height) / kGuestHeight;
    const float output_guest_width = wide_guest_width(output_width, scale);
    using bumble::text_overlay::TextKind;
    using bumble::text_overlay::HorizontalAnchor;
    const bool gameplay = observation.kind == TextKind::GameplayHud;
    const bool panel_layout =
        observation.horizontal_anchor == HorizontalAnchor::Panel;
    const bool panel_wrap = panel_layout &&
        (observation.kind == TextKind::Script ||
         observation.kind == TextKind::Briefing) &&
        output_guest_width > kGuestWidth + 0.01f;
    const float panel_scale = panel_layout
        ? wide_panel_font_scale(output_guest_width)
        : 1.0f;
    const float authored_em_size = gameplay
        ? kGameplayFontEmPixelsPerGuestPixel
        : kBriefingFontEmPixelsPerGuestPixel;
    const float authored_outline_width =
        (observation.kind == TextKind::Menu ||
         observation.kind == TextKind::HighScores ||
         observation.kind == TextKind::Editor ||
         observation.kind == TextKind::Completion)
        ? kMenuOutlinePixelsPerGuestPixel
        : observation.kind == TextKind::Credits
            ? kCreditsOutlinePixelsPerGuestPixel
            : gameplay
                ? kGameplayOutlinePixelsPerGuestPixel
                : kBriefingOutlinePixelsPerGuestPixel;
    const float em_size = std::max(
        6.0f,
        authored_em_size * observation.screen_scale * panel_scale * scale
    );
    const float outline_width = std::max(
        0.75f,
        authored_outline_width * observation.screen_scale * panel_scale * scale
    );
    float layout_width = std::numeric_limits<float>::max();
    if (panel_wrap) {
        const float margin = wide_panel_margin(output_guest_width);
        const float text_left = std::max(
            margin,
            static_cast<float>(observation.x) * observation.screen_scale
        );
        const float layout_guest_width = observation.panel_reveal
            ? static_cast<float>(observation.width) +
                std::max(0.0f, output_guest_width - kGuestWidth)
            : observation.kind == TextKind::Briefing
            ? kBriefingPanelAuthoredWidth +
                std::max(0.0f, output_guest_width - kGuestWidth) -
                kBriefingPanelTextInset * 2.0f
            : output_guest_width - text_left - margin;
        layout_width =
            std::max(1.0f, layout_guest_width * scale);
    }

    struct PlacedGlyph {
        int codepoint = 0;
        float x = 0.0f;
        float baseline = 0.0f;
        size_t source_index = 0u;
    };
    std::vector<PlacedGlyph> glyphs;
    const float font_scale = stbtt_ScaleForPixelHeight(&g_state.font, em_size);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&g_state.font, &ascent, &descent, &line_gap);
    const float baseline_offset = static_cast<float>(ascent) * font_scale;
    const float line_height =
        static_cast<float>(ascent - descent + line_gap) * font_scale;
    float cursor_x = 0.0f;
    float baseline = baseline_offset;
    int previous = 0;
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    const auto normalized_codepoint = [](unsigned char byte) {
        const int codepoint = byte;
        return codepoint < 0x20 || codepoint > 0x7E ? '?' : codepoint;
    };
    const auto codepoint_width = [&](int left, int codepoint) {
        int advance = 0;
        int left_bearing = 0;
        stbtt_GetCodepointHMetrics(
            &g_state.font,
            codepoint,
            &advance,
            &left_bearing
        );
        const float kerning = left != 0
            ? static_cast<float>(stbtt_GetCodepointKernAdvance(
                &g_state.font,
                left,
                codepoint
            )) * font_scale
            : 0.0f;
        return kerning + static_cast<float>(advance) * font_scale;
    };
    const auto begin_next_line = [&]() {
        cursor_x = 0.0f;
        baseline += line_height;
        previous = 0;
    };

    for (size_t index = 0; index < observation.text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(
            observation.text[index]
        );
        int codepoint = byte;
        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            begin_next_line();
            continue;
        }
        codepoint = normalized_codepoint(byte);

        if (panel_wrap && codepoint == ' ') {
            if (cursor_x <= 0.0f) {
                previous = 0;
                continue;
            }
            const float space_width = codepoint_width(previous, codepoint);
            if (cursor_x + space_width > layout_width) {
                begin_next_line();
                continue;
            }
            cursor_x += space_width;
            previous = codepoint;
            continue;
        }

        const bool begins_word = panel_wrap && codepoint != ' ' &&
            (index == 0u || observation.text[index - 1u] == ' ' ||
             observation.text[index - 1u] == '\n' ||
             observation.text[index - 1u] == '\r');
        if (begins_word) {
            float word_width = 0.0f;
            int word_previous = previous;
            for (size_t word_index = index;
                 word_index < observation.text.size();
                 ++word_index) {
                const unsigned char word_byte = static_cast<unsigned char>(
                    observation.text[word_index]
                );
                if (word_byte == ' ' || word_byte == '\n' ||
                    word_byte == '\r') {
                    break;
                }
                const int word_codepoint = normalized_codepoint(word_byte);
                word_width += codepoint_width(word_previous, word_codepoint);
                word_previous = word_codepoint;
            }
            if (cursor_x > 0.0f &&
                cursor_x + word_width > layout_width) {
                begin_next_line();
            }
        }

        int advance = 0;
        int left_bearing = 0;
        stbtt_GetCodepointHMetrics(
            &g_state.font,
            codepoint,
            &advance,
            &left_bearing
        );
        const float kerning = previous != 0
            ? static_cast<float>(stbtt_GetCodepointKernAdvance(
                &g_state.font,
                previous,
                codepoint
            )) * font_scale
            : 0.0f;
        const float glyph_advance = static_cast<float>(advance) * font_scale;
        if (panel_wrap && cursor_x > 0.0f &&
            cursor_x + kerning + glyph_advance > layout_width) {
            begin_next_line();
        } else {
            cursor_x += kerning;
        }
        if (codepoint != ' ') {
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetCodepointBitmapBox(
                &g_state.font,
                codepoint,
                font_scale,
                font_scale,
                &x0,
                &y0,
                &x1,
                &y1
            );
            glyphs.push_back({codepoint, cursor_x, baseline, index});
            min_x = std::min(min_x, cursor_x + static_cast<float>(x0));
            min_y = std::min(min_y, baseline + static_cast<float>(y0));
            max_x = std::max(max_x, cursor_x + static_cast<float>(x1));
            max_y = std::max(max_y, baseline + static_cast<float>(y1));
        }
        cursor_x += glyph_advance;
        previous = codepoint;
    }
    if (glyphs.empty() || max_x <= min_x || max_y <= min_y) {
        return false;
    }

    const int outline_radius = std::max(1, static_cast<int>(
        std::ceil(outline_width)
    ));
    const uint32_t padding = kRasterPadding +
        static_cast<uint32_t>(outline_radius);
    const uint32_t width = std::max<uint32_t>(
        1u,
        static_cast<uint32_t>(std::ceil(max_x - min_x)) + padding * 2u
    );
    const uint32_t height = std::max<uint32_t>(
        1u,
        static_cast<uint32_t>(std::ceil(max_y - min_y)) + padding * 2u
    );
    const uint64_t maximum_size = g_state.device->getCapabilities().maxTextureSize;
    if (width > maximum_size || height > maximum_size) {
        return false;
    }
    if (measure_only) {
        return true;
    }
    std::vector<uint8_t> face(static_cast<size_t>(width) * height, 0u);
    if (observation.panel_reveal) {
        result.reveal.resize(observation.text.size() + 1u);
    }
    for (const PlacedGlyph& glyph : glyphs) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(
            &g_state.font,
            glyph.codepoint,
            font_scale,
            font_scale,
            &x0,
            &y0,
            &x1,
            &y1
        );
        const int glyph_width = x1 - x0;
        const int glyph_height = y1 - y0;
        if (glyph_width <= 0 || glyph_height <= 0) {
            continue;
        }
        std::vector<uint8_t> bitmap(
            static_cast<size_t>(glyph_width) * glyph_height
        );
        stbtt_MakeCodepointBitmap(
            &g_state.font,
            bitmap.data(),
            glyph_width,
            glyph_height,
            glyph_width,
            font_scale,
            font_scale,
            glyph.codepoint
        );
        const int destination_x = static_cast<int>(std::floor(
            glyph.x + static_cast<float>(x0) - min_x
        )) + static_cast<int>(padding);
        const int destination_y = static_cast<int>(std::floor(
            glyph.baseline + static_cast<float>(y0) - min_y
        )) + static_cast<int>(padding);
        if (observation.panel_reveal) {
            const float row_top = std::max(0.0f, std::floor(
                glyph.baseline - baseline_offset - min_y + padding));
            result.reveal[glyph.source_index + 1u] = {
                static_cast<float>(destination_x + glyph_width + outline_radius),
                row_top,
                std::min(static_cast<float>(height), std::ceil(row_top + line_height))
            };
        }
        for (int y = 0; y < glyph_height; ++y) {
            for (int x = 0; x < glyph_width; ++x) {
                const int target_x = destination_x + x;
                const int target_y = destination_y + y;
                if (target_x < 0 || target_y < 0 ||
                    target_x >= static_cast<int>(width) ||
                    target_y >= static_cast<int>(height)) {
                    continue;
                }
                uint8_t& destination = face[
                    static_cast<size_t>(target_y) * width +
                    static_cast<size_t>(target_x)
                ];
                destination = std::max(
                    destination,
                    bitmap[static_cast<size_t>(y) * glyph_width + x]
                );
            }
        }
    }
    std::vector<uint8_t> outline(face.size(), 0u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t alpha = face[static_cast<size_t>(y) * width + x];
            if (alpha == 0u) {
                continue;
            }
            for (int dy = -outline_radius; dy <= outline_radius; ++dy) {
                for (int dx = -outline_radius; dx <= outline_radius; ++dx) {
                    if (dx * dx + dy * dy >
                        outline_radius * outline_radius) {
                        continue;
                    }
                    const int target_x = static_cast<int>(x) + dx;
                    const int target_y = static_cast<int>(y) + dy;
                    if (target_x < 0 || target_y < 0 ||
                        target_x >= static_cast<int>(width) ||
                        target_y >= static_cast<int>(height)) {
                        continue;
                    }
                    uint8_t& destination = outline[
                        static_cast<size_t>(target_y) * width +
                        static_cast<size_t>(target_x)
                    ];
                    destination = std::max(destination, alpha);
                }
            }
        }
    }

    const auto component = [](uint32_t rgba, uint32_t shift) {
        return static_cast<uint8_t>((rgba >> shift) & 0xFFu);
    };
    result.rgba.resize(static_cast<size_t>(width) * height * 4u);
    for (size_t pixel = 0; pixel < face.size(); ++pixel) {
        const bool is_face = face[pixel] != 0u;
        const uint32_t color = is_face
            ? observation.face_rgba
            : observation.outline_rgba;
        const uint8_t coverage = is_face ? face[pixel] : outline[pixel];
        result.rgba[pixel * 4u + 0u] = component(color, 24u);
        result.rgba[pixel * 4u + 1u] = component(color, 16u);
        result.rgba[pixel * 4u + 2u] = component(color, 8u);
        result.rgba[pixel * 4u + 3u] = static_cast<uint8_t>(
            (static_cast<uint32_t>(coverage) * component(color, 0u)) / 255u
        );
    }
    result.width = width;
    result.height = height;
    for (size_t index = 1u; index < result.reveal.size(); ++index) {
        if (result.reveal[index].bottom == 0.0f) {
            result.reveal[index] = result.reveal[index - 1u];
        }
    }
    return true;
}

constexpr float honeycomb_cell_fill(
    float total_fill,
    uint32_t cell_count,
    uint32_t cell_index
) {
    const float cell_fill = total_fill * static_cast<float>(cell_count) -
        static_cast<float>(cell_index);
    return cell_fill <= 0.0f ? 0.0f : cell_fill >= 1.0f ? 1.0f : cell_fill;
}

static_assert(honeycomb_cell_fill(0.75f, 10u, 6u) == 1.0f);
static_assert(honeycomb_cell_fill(0.75f, 10u, 7u) == 0.5f);
static_assert(honeycomb_cell_fill(0.75f, 10u, 8u) == 0.0f);
static_assert(honeycomb_cell_fill(0.50f, 5u, 2u) == 0.5f);

bool rasterize_health_honeycomb(
    const bumble::text_overlay::Observation& observation,
    uint32_t output_height,
    RasterizedText& result
) {
    result = {};
    if (output_height == 0u || observation.cell_count == 0u ||
        observation.cell_count > 10u) {
        return false;
    }
    const float scale = static_cast<float>(output_height) / kGuestHeight;
    const uint32_t cell_width = std::max(
        8u,
        static_cast<uint32_t>(std::lround(6.0f * scale))
    );
    const uint32_t cell_height = std::max(
        10u,
        static_cast<uint32_t>(std::lround(8.0f * scale))
    );
    const uint32_t gap = std::max(
        2u,
        static_cast<uint32_t>(std::lround(scale))
    );
    const uint32_t padding = 2u;
    const uint32_t border = std::max(
        1u,
        static_cast<uint32_t>(std::lround(scale * 0.55f))
    );
    result.width = padding * 2u + observation.cell_count * cell_width +
        (observation.cell_count - 1u) * gap;
    result.height = padding * 2u + cell_height;
    result.rgba.assign(
        static_cast<size_t>(result.width) * result.height * 4u,
        0u
    );

    const bool critical = observation.fill_fraction <= 0.20f;
    for (uint32_t cell = 0u; cell < observation.cell_count; ++cell) {
        const float fill = honeycomb_cell_fill(
            observation.fill_fraction,
            observation.cell_count,
            cell
        );
        const uint32_t origin_x = padding + cell * (cell_width + gap);
        for (uint32_t y = 0u; y < cell_height; ++y) {
            const float vertical =
                (static_cast<float>(y) + 0.5f) /
                static_cast<float>(cell_height);
            const float edge = std::abs(vertical - 0.5f) * 2.0f;
            const float left = edge * static_cast<float>(cell_width) * 0.24f;
            const float right = static_cast<float>(cell_width) - left;
            for (uint32_t x = 0u; x < cell_width; ++x) {
                const float pixel_x = static_cast<float>(x) + 0.5f;
                if (pixel_x < left || pixel_x >= right) {
                    continue;
                }
                const bool outline = y < border || y + border >= cell_height ||
                    pixel_x < left + static_cast<float>(border) ||
                    pixel_x >= right - static_cast<float>(border);
                const bool filled = !outline && fill > 0.0f &&
                    vertical >= 1.0f - fill;
                const uint8_t red = outline ? 55u : filled
                    ? (critical ? 235u : 255u)
                    : 65u;
                const uint8_t green = outline ? 34u : filled
                    ? (critical ? 55u : 177u)
                    : 55u;
                const uint8_t blue = outline ? 7u : filled
                    ? (critical ? 18u : 28u)
                    : 38u;
                const uint8_t alpha = outline ? 255u : filled ? 245u : 210u;
                const size_t pixel = (
                    static_cast<size_t>(padding + y) * result.width +
                    origin_x + x
                ) * 4u;
                result.rgba[pixel + 0u] = red;
                result.rgba[pixel + 1u] = green;
                result.rgba[pixel + 2u] = blue;
                result.rgba[pixel + 3u] = alpha;
            }
        }
    }
    return true;
}

std::unique_ptr<RenderShader> create_vertex_shader(
    RenderDevice* device,
    RenderShaderFormat format
) {
    switch (format) {
    case RenderShaderFormat::DXIL:
        return device->createShader(
            BumbleTextOverlayVSDXIL,
            sizeof(BumbleTextOverlayVSDXIL),
            "VSMain",
            format
        );
    case RenderShaderFormat::SPIRV:
        return device->createShader(
            BumbleTextOverlayVSSPIRV,
            sizeof(BumbleTextOverlayVSSPIRV),
            "VSMain",
            format
        );
    default:
        return nullptr;
    }
}

std::unique_ptr<RenderShader> create_pixel_shader(
    RenderDevice* device,
    RenderShaderFormat format
) {
    switch (format) {
    case RenderShaderFormat::DXIL:
        return device->createShader(
            BumbleTextOverlayPSDXIL,
            sizeof(BumbleTextOverlayPSDXIL),
            "PSMain",
            format
        );
    case RenderShaderFormat::SPIRV:
        return device->createShader(
            BumbleTextOverlayPSSPIRV,
            sizeof(BumbleTextOverlayPSSPIRV),
            "PSMain",
            format
        );
    default:
        return nullptr;
    }
}

bool same_slot(
    const RenderedSlot& slot,
    const bumble::text_overlay::Observation& observation
) {
    if (slot.observation.kind != observation.kind ||
        slot.observation.primitive != observation.primitive) {
        return false;
    }
    if (slot.observation.stable_slot != 0u ||
        observation.stable_slot != 0u) {
        return slot.observation.stable_slot != 0u &&
            slot.observation.stable_slot == observation.stable_slot;
    }
    return slot.observation.x == observation.x &&
        slot.observation.y == observation.y;
}

float mapped_guest_x(
    const bumble::text_overlay::Observation& observation,
    uint32_t output_width,
    float guest_scale
) {
    using bumble::text_overlay::HorizontalAnchor;
    const float output_guest_width = wide_guest_width(
        output_width,
        guest_scale
    );
    const float extra_width = std::max(
        0.0f,
        output_guest_width - kGuestWidth
    );
    const float x = static_cast<float>(observation.x);
    switch (observation.horizontal_anchor) {
    case HorizontalAnchor::Left:
        return x * observation.screen_scale;
    case HorizontalAnchor::Center:
    case HorizontalAnchor::CenterText:
        return kGuestWidth * 0.5f + extra_width * 0.5f +
            (x - kGuestWidth * 0.5f) * observation.screen_scale;
    case HorizontalAnchor::Right:
        return kGuestWidth + extra_width +
            (x - kGuestWidth) * observation.screen_scale;
    case HorizontalAnchor::RightInset:
    case HorizontalAnchor::RightTextInset:
        return output_guest_width - x;
    case HorizontalAnchor::Panel: {
        if (output_guest_width <= kGuestWidth + 0.01f) {
            return x;
        }
        const float margin = wide_panel_margin(output_guest_width);
        return std::max(margin, x * observation.screen_scale);
    }
    case HorizontalAnchor::Authored:
        return x;
    }
    return x;
}

float mapped_guest_y(
    const bumble::text_overlay::Observation& observation
) {
    using bumble::text_overlay::VerticalAnchor;
    const float y = static_cast<float>(observation.y);
    switch (observation.vertical_anchor) {
    case VerticalAnchor::Top:
        return y * observation.screen_scale;
    case VerticalAnchor::Center:
        return kGuestHeight * 0.5f +
            (y - kGuestHeight * 0.5f) * observation.screen_scale;
    case VerticalAnchor::Bottom:
        return kGuestHeight +
            (y - kGuestHeight) * observation.screen_scale;
    case VerticalAnchor::Authored:
        return y;
    }
    return y;
}

} // namespace

bool bumble::text_overlay::supports_briefing_layout(const Observation& observation) {
    std::scoped_lock lock(g_state.mutex);
    if (!g_state.font_ready || g_state.device == nullptr || g_state.swap_chain == nullptr) {
        return false;
    }
    static Observation cached;
    static uint32_t cached_width = 0u;
    static uint32_t cached_height = 0u;
    static uint64_t cached_maximum = 0u;
    static bool supported = false;
    const uint32_t width = g_state.output_width;
    const uint32_t height = g_state.output_height;
    const uint64_t maximum = g_state.device->getCapabilities().maxTextureSize;
    if (width == 0u || height == 0u) {
        return false;
    }
    if (cached_width != width || cached_height != height || cached_maximum != maximum ||
        cached.text != observation.text || cached.x != observation.x ||
        cached.width != observation.width || cached.screen_scale != observation.screen_scale) {
        cached = observation;
        cached_width = width;
        cached_height = height;
        cached_maximum = maximum;
        RasterizedText measured;
        supported = rasterize_text(observation, width, height, measured, true);
    }
    return supported;
}

bool bumble::text_overlay::configure_menu_background(
    const std::filesystem::path& asset_directory
) {
    std::scoped_lock lock(g_state.mutex);
    g_state.menu_background_bytes.clear();
    g_state.main_menu_background_bytes.assign(
        BumbleMenuBackground,
        BumbleMenuBackground + BumbleMenuBackground_size
    );
    g_state.menu_background_texture.reset();
    g_state.main_menu_background_texture.reset();
    g_state.menu_background_upload_buffer.reset();
    g_state.main_menu_background_upload_buffer.reset();
    g_state.menu_background_descriptor_set.reset();
    g_state.main_menu_background_descriptor_set.reset();
    g_state.menu_background_configured = false;
    g_state.menu_background_upload_failure_logged = false;
    g_state.main_menu_background_upload_failure_logged = false;
    g_state.menu_background_presented_logged = false;
    g_state.main_menu_background_presented_logged = false;
    if (!load_menu_background_asset(asset_directory)) {
        std::fprintf(
            stderr,
            "BUMBLE_TEXT_OVERLAY stage=menu_background_rejected path=%s\n",
            asset_directory.string().c_str()
        );
        std::fflush(stderr);
        return false;
    }
    g_state.menu_background_configured = true;
    std::fprintf(
        stderr,
        "BUMBLE_TEXT_OVERLAY stage=menu_background_ready path=%s bytes=%zu"
        " source=user_authored width=%" PRIu32 " height=%" PRIu32 "\n",
        asset_directory.string().c_str(),
        g_state.menu_background_bytes.size(),
        kMenuBackgroundWidth,
        kMenuBackgroundHeight
    );
    std::fflush(stderr);
    return true;
}

bool bumble::text_overlay::menu_backdrop_available(bool main_menu) {
    std::scoped_lock lock(g_state.mutex);
    return g_state.menu_background_configured &&
        !(main_menu ? g_state.main_menu_background_bytes :
            g_state.menu_background_bytes).empty();
}

bool bumble::text_overlay::bind_renderer(
    RenderInterface* render_interface,
    RenderDevice* device,
    const RT64::ShaderLibrary* shader_library,
    RenderSwapChain* swap_chain
) {
    std::scoped_lock lock(g_state.mutex);
    if (render_interface == nullptr || device == nullptr ||
        shader_library == nullptr || swap_chain == nullptr) {
        return false;
    }

    const auto* font_bytes = reinterpret_cast<const unsigned char*>(
        BumbleTextFont
    );
    const int font_offset = stbtt_GetFontOffsetForIndex(font_bytes, 0);
    if (font_offset < 0 ||
        !stbtt_InitFont(&g_state.font, font_bytes, font_offset)) {
        g_state.font_ready = false;
        return false;
    }
    g_state.font_ready = true;

    const RenderShaderFormat shader_format =
        render_interface->getCapabilities().shaderFormat;
    std::unique_ptr<RenderShader> vertex_shader =
        create_vertex_shader(device, shader_format);
    std::unique_ptr<RenderShader> pixel_shader =
        create_pixel_shader(device, shader_format);
    if (vertex_shader == nullptr || pixel_shader == nullptr) {
        g_state.font_ready = false;
        return false;
    }

    const RenderSampler* sampler =
        shader_library->samplerLibrary.linear.borderBorder.get();
    RT64::VideoInterfaceDescriptorSet descriptor_layout(sampler);
    RenderPipelineLayoutBuilder layout_builder;
    layout_builder.begin();
    layout_builder.addPushConstant(
        0,
        0,
        sizeof(TextOverlayConstants),
        RenderShaderStageFlag::PIXEL
    );
    layout_builder.addDescriptorSet(descriptor_layout);
    layout_builder.end();
    g_state.pipeline_layout = layout_builder.create(device);
    if (g_state.pipeline_layout == nullptr) {
        g_state.font_ready = false;
        return false;
    }

    RenderGraphicsPipelineDesc pipeline_description;
    pipeline_description.pipelineLayout = g_state.pipeline_layout.get();
    pipeline_description.vertexShader = vertex_shader.get();
    pipeline_description.pixelShader = pixel_shader.get();
    pipeline_description.renderTargetFormat[0] =
        RenderFormat::B8G8R8A8_UNORM;
    pipeline_description.renderTargetBlend[0] =
        RenderBlendDesc::AlphaBlend();
    pipeline_description.renderTargetCount = 1;
    g_state.pipeline = device->createGraphicsPipeline(pipeline_description);
    if (g_state.pipeline == nullptr) {
        g_state.pipeline_layout.reset();
        g_state.font_ready = false;
        return false;
    }

    g_state.render_interface = render_interface;
    g_state.device = device;
    g_state.shader_library = shader_library;
    g_state.swap_chain = swap_chain;
    g_state.ready = true;
    g_state.presented_logged = false;
    g_state.wide_panel_layout_logged = false;
    set_renderer_ready(true);
    std::fprintf(
        stderr,
        "BUMBLE_TEXT_OVERLAY stage=renderer_ready"
        " font=Roboto_Medium source=embedded"
        " rasterization=output_resolution cross_platform=1"
        " outline=bitmap_dilation panel_side_fill=disabled"
        " menu_backdrop=user_authored_full_viewport"
        " guest_font_asset_packaged=0\n"
    );
    std::fflush(stderr);
    return true;
}

void bumble::text_overlay::draw(
    RenderCommandList* command_list,
    RenderFramebuffer* framebuffer,
    uint64_t submission_id
) {
    g_last_draw_selected_weapon.store(UINT32_MAX, std::memory_order_release);
    const std::vector<Observation> observations = active_observations();
    if (std::ranges::any_of(observations, [](const Observation& observation) {
            return observation.primitive == OverlayPrimitive::CompletionBackdrop;
        })) {
        bumble::game_completion_screen::draw(command_list, framebuffer);
    }
    if (observations.empty()) {
        return;
    }

    std::scoped_lock lock(g_state.mutex);
    if (!g_state.ready || command_list == nullptr || framebuffer == nullptr ||
        g_state.device == nullptr || g_state.swap_chain == nullptr ||
        g_state.pipeline == nullptr || g_state.pipeline_layout == nullptr) {
        return;
    }
    const uint32_t output_width = framebuffer->getWidth();
    const uint32_t output_height = framebuffer->getHeight();
    g_state.output_width = output_width;
    g_state.output_height = output_height;
    if (output_width == 0u || output_height == 0u) {
        return;
    }

    for (auto slot = g_state.slots.begin();
         slot != g_state.slots.end();) {
        const bool still_active = std::any_of(
            observations.begin(),
            observations.end(),
            [&slot](const Observation& observation) {
                return same_slot(*slot, observation);
            }
        );
        if (!still_active) {
            retire_slot_resources(*slot);
            slot = g_state.slots.erase(slot);
        }
        else {
            ++slot;
        }
    }
    const float guest_scale =
        static_cast<float>(output_height) / kGuestHeight;
    const RenderRect full_scissor(
        0,
        0,
        static_cast<int32_t>(output_width),
        static_cast<int32_t>(output_height)
    );
    command_list->setFramebuffer(framebuffer);
    command_list->setScissors(full_scissor);
    command_list->setPipeline(g_state.pipeline.get());
    command_list->setGraphicsPipelineLayout(g_state.pipeline_layout.get());
    command_list->setVertexBuffers(0, nullptr, 0, nullptr);

    uint32_t drawn = 0u;
    uint32_t selected_weapon_drawn = UINT32_MAX;
    uint32_t editor_drawn = 0u;
    float editor_x = 0.0f;
    float editor_y = 0.0f;
    float editor_width = 0.0f;
    float editor_height = 0.0f;
    for (const Observation& observation : observations) {
        if (observation.primitive == OverlayPrimitive::CompletionBackdrop) {
            continue;
        }
        const bool main_menu_backdrop = observation.primitive ==
            bumble::text_overlay::OverlayPrimitive::MainMenuBackdrop;
        if (observation.primitive ==
                bumble::text_overlay::OverlayPrimitive::MenuBackdrop ||
            main_menu_backdrop) {
            auto& background_bytes = main_menu_backdrop
                ? g_state.main_menu_background_bytes
                : g_state.menu_background_bytes;
            auto& background_texture = main_menu_backdrop
                ? g_state.main_menu_background_texture
                : g_state.menu_background_texture;
            auto& background_upload_buffer = main_menu_backdrop
                ? g_state.main_menu_background_upload_buffer
                : g_state.menu_background_upload_buffer;
            auto& background_descriptor_set = main_menu_backdrop
                ? g_state.main_menu_background_descriptor_set
                : g_state.menu_background_descriptor_set;
            bool& upload_failure_logged = main_menu_backdrop
                ? g_state.main_menu_background_upload_failure_logged
                : g_state.menu_background_upload_failure_logged;
            bool& presented_logged = main_menu_backdrop
                ? g_state.main_menu_background_presented_logged
                : g_state.menu_background_presented_logged;
            if (!g_state.menu_background_configured ||
                background_bytes.empty()) {
                continue;
            }
            if (background_texture == nullptr) {
                background_texture.reset(
                    RT64::TextureCache::loadTextureFromBytes(
                        g_state.device,
                        command_list,
                        background_bytes,
                        background_upload_buffer
                    )
                );
                if (background_texture == nullptr ||
                    background_texture->width !=
                        kMenuBackgroundWidth ||
                    background_texture->height !=
                        kMenuBackgroundHeight) {
                    background_texture.reset();
                    if (!upload_failure_logged) {
                        upload_failure_logged = true;
                        std::fprintf(
                            stderr,
                            "BUMBLE_TEXT_OVERLAY"
                            " stage=%s_background_gpu_upload_failed\n",
                            main_menu_backdrop ? "main_menu" : "menu"
                        );
                        std::fflush(stderr);
                    }
                    continue;
                }
                command_list->barriers(
                    RenderBarrierStage::GRAPHICS,
                    RenderTextureBarrier(
                        background_texture->texture.get(),
                        RenderTextureLayout::SHADER_READ
                    )
                );
            }
            if (background_descriptor_set == nullptr) {
                const RenderSampler* sampler =
                    g_state.shader_library->samplerLibrary.linear
                        .borderBorder.get();
                background_descriptor_set =
                    std::make_unique<RT64::VideoInterfaceDescriptorSet>(
                        sampler,
                        g_state.device
                    );
                background_descriptor_set->setTexture(
                    background_descriptor_set->gInput,
                    background_texture->texture.get(),
                    RenderTextureLayout::SHADER_READ
                );
            }

            const float target_width = static_cast<float>(output_width);
            const float target_height = static_cast<float>(output_height);
            const float source_aspect =
                static_cast<float>(kMenuBackgroundWidth) /
                static_cast<float>(kMenuBackgroundHeight);
            const float target_aspect = target_width / target_height;
            float draw_width = target_width;
            float draw_height = target_height;
            float draw_x = 0.0f;
            float draw_y = 0.0f;
            if (target_aspect > source_aspect) {
                draw_height = target_width / source_aspect;
                draw_y = (target_height - draw_height) * 0.5f;
            }
            else if (target_aspect < source_aspect) {
                draw_width = target_height * source_aspect;
                draw_x = (target_width - draw_width) * 0.5f;
            }
            command_list->setViewports(RenderViewport(
                draw_x,
                draw_y,
                draw_width,
                draw_height
            ));
            command_list->setGraphicsDescriptorSet(
                background_descriptor_set->get(),
                0
            );
            const TextOverlayConstants constants{};
            command_list->setGraphicsPushConstants(0, &constants);
            command_list->drawInstanced(3, 1, 0, 0);
            if (!presented_logged) {
                presented_logged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_TEXT_OVERLAY"
                    " stage=%s_background_presented"
                    " source=user_authored full_framebuffer=1"
                    " width=%" PRIu32 " height=%" PRIu32
                    " scale=cover_preserve_aspect corners_covered=1\n",
                    main_menu_backdrop ? "main_menu" : "menu",
                    output_width,
                    output_height
                );
                std::fflush(stderr);
            }
            ++drawn;
            continue;
        }

        auto slot = std::find_if(
            g_state.slots.begin(),
            g_state.slots.end(),
            [&observation](const RenderedSlot& candidate) {
                return same_slot(candidate, observation);
            }
        );
        if (slot == g_state.slots.end()) {
            g_state.slots.emplace_back();
            slot = std::prev(g_state.slots.end());
        }
        if (slot->observation.revision != observation.revision ||
            slot->output_width != output_width ||
            slot->output_height != output_height) {
            slot->observation = observation;
            slot->output_width = output_width;
            slot->output_height = output_height;
            retire_slot_resources(*slot);
            if (observation.horizontal_anchor ==
                    bumble::text_overlay::HorizontalAnchor::Panel &&
                output_width > output_height * 4u / 3u &&
                !g_state.wide_panel_layout_logged) {
                g_state.wide_panel_layout_logged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_TEXT_OVERLAY stage=wide_panel_text_layout"
                    " output=%" PRIu32 "x%" PRIu32
                    " script_reflow=full_width briefing_wrap=expanded\n",
                    output_width,
                    output_height
                );
                std::fflush(stderr);
            }
            if (observation.primitive ==
                    bumble::text_overlay::OverlayPrimitive::Panel) {
                slot->pixels = {
                    {
                        static_cast<uint8_t>(observation.face_rgba >> 24u),
                        static_cast<uint8_t>(observation.face_rgba >> 16u),
                        static_cast<uint8_t>(observation.face_rgba >> 8u),
                        static_cast<uint8_t>(observation.face_rgba)
                    },
                    1u,
                    1u
                };
            }
            else if (observation.primitive ==
                    bumble::text_overlay::OverlayPrimitive::HealthHoneycomb) {
                if (!rasterize_health_honeycomb(
                        observation,
                        output_height,
                        slot->pixels
                    )) {
                    slot->pixels = {};
                    continue;
                }
            }
            else if (!rasterize_text(
                         observation,
                         output_width,
                         output_height,
                         slot->pixels
                     )) {
                slot->pixels = {};
                continue;
            }
        }
        if (slot->pixels.rgba.empty()) {
            continue;
        }
        if (observation.panel_reveal && observation.visible_characters == 0u) {
            continue;
        }
        const bool menu_backdrop = observation.kind == TextKind::Menu &&
            (observation.primitive == OverlayPrimitive::MenuBackdrop ||
                observation.primitive == OverlayPrimitive::MainMenuBackdrop);
        const uint8_t alpha = observation.kind == TextKind::Menu &&
                !menu_backdrop
            ? bumble::graphics_options::menu_transition_alpha()
            : 255u;
        if (slot->texture.texture == nullptr) {
            RT64::TextureCache::setRGBA32(
                &slot->texture,
                g_state.device,
                command_list,
                slot->pixels.rgba.data(),
                slot->pixels.rgba.size(),
                slot->pixels.width,
                slot->pixels.height,
                slot->pixels.width * 4u,
                slot->upload_buffer
            );
            if (slot->texture.texture == nullptr) {
                continue;
            }
            command_list->barriers(
                RenderBarrierStage::GRAPHICS,
                RenderTextureBarrier(
                    slot->texture.texture.get(),
                    RenderTextureLayout::SHADER_READ
                )
            );
        }

        if (slot->descriptor_set == nullptr) {
            const RenderSampler* sampler =
                g_state.shader_library->samplerLibrary.linear.borderBorder.get();
            slot->descriptor_set =
                std::make_unique<RT64::VideoInterfaceDescriptorSet>(
                    sampler,
                    g_state.device
                );
            slot->descriptor_set->setTexture(
                slot->descriptor_set->gInput,
                slot->texture.texture.get(),
                RenderTextureLayout::SHADER_READ
            );
        }
        float draw_x = std::round(
            mapped_guest_x(observation, output_width, guest_scale) *
                guest_scale
        );
        float draw_y = std::round(
            mapped_guest_y(observation) * guest_scale
        );
        float draw_width = static_cast<float>(slot->pixels.width);
        float draw_height = static_cast<float>(slot->pixels.height);
        const float panel_top = draw_y;
        const float panel_height = static_cast<float>(observation.height) * guest_scale;
        RasterizedText::RevealBoundary reveal{};
        if (observation.panel_reveal && !slot->pixels.reveal.empty()) {
            const size_t visible = std::min<size_t>(observation.visible_characters,
                slot->pixels.reveal.size() - 1u);
            reveal = slot->pixels.reveal[visible];
            draw_y -= std::max(0.0f, reveal.bottom - panel_height);
        }
        if (observation.horizontal_anchor ==
                bumble::text_overlay::HorizontalAnchor::CenterText) {
            draw_x -= draw_width * 0.5f;
        }
        else if (observation.horizontal_anchor ==
                bumble::text_overlay::HorizontalAnchor::RightTextInset) {
            draw_x -= draw_width;
        }
        if (observation.primitive ==
                bumble::text_overlay::OverlayPrimitive::Panel) {
            draw_width = static_cast<float>(observation.width) *
                observation.screen_scale * guest_scale;
            draw_height = static_cast<float>(observation.height) *
                observation.screen_scale * guest_scale;
        }
        const RenderViewport viewport(
            draw_x,
            draw_y,
            draw_width,
            draw_height
        );
        if (observation.kind == TextKind::Editor &&
            editor_drawn == 0u) {
            editor_x = draw_x;
            editor_y = draw_y;
            editor_width = draw_width;
            editor_height = draw_height;
        }
        command_list->setViewports(viewport);
        command_list->setGraphicsDescriptorSet(
            slot->descriptor_set->get(),
            0
        );
        const TextOverlayConstants constants{
            static_cast<float>(alpha) / 255.0f
        };
        command_list->setGraphicsPushConstants(0, &constants);
        if (observation.panel_reveal) {
            const float panel_width = (static_cast<float>(observation.width) +
                std::max(0.0f, static_cast<float>(output_width) / guest_scale - kGuestWidth)) * guest_scale;
            const auto draw_region = [&](float right, float top, float bottom) {
                const int32_t left_pixel = std::max(0, static_cast<int32_t>(draw_x));
                const int32_t top_pixel = std::max(0, static_cast<int32_t>(std::ceil(std::max(panel_top, top))));
                const int32_t right_pixel = std::min(static_cast<int32_t>(output_width),
                    static_cast<int32_t>(std::ceil(std::min(draw_x + panel_width, right))));
                const int32_t bottom_pixel = std::min(static_cast<int32_t>(output_height),
                    static_cast<int32_t>(std::ceil(std::min(panel_top + panel_height, bottom))));
                if (right_pixel > left_pixel && bottom_pixel > top_pixel) {
                    command_list->setScissors(RenderRect(left_pixel, top_pixel, right_pixel, bottom_pixel));
                    command_list->drawInstanced(3, 1, 0, 0);
                }
            };
            if (observation.visible_characters >= observation.text.size()) {
                draw_region(draw_x + draw_width, draw_y, draw_y + draw_height);
            }
            else {
                draw_region(draw_x + draw_width, draw_y, draw_y + reveal.top);
                draw_region(draw_x + reveal.right, draw_y + reveal.top, draw_y + reveal.bottom);
            }
            command_list->setScissors(full_scissor);
        }
        else {
            command_list->drawInstanced(3, 1, 0, 0);
        }
        slot->last_submission_id = submission_id;
        if (observation.kind == TextKind::GameplayHud &&
            observation.primitive == OverlayPrimitive::Text &&
            observation.text.size() > 2u && observation.text[0] == '>' &&
            observation.text[1] == ' ') {
            const std::string_view label(
                observation.text.data() + 2u,
                observation.text.size() - 2u
            );
            for (uint32_t weapon = 0u;
                 weapon < bumble::weapon_system::weapon_count();
                 ++weapon) {
                if (label == bumble::weapon_system::hud_label(weapon)) {
                    selected_weapon_drawn = weapon;
                    break;
                }
            }
        }
        ++drawn;
        editor_drawn += observation.kind == TextKind::Editor ? 1u : 0u;
    }

    if (editor_drawn != 0u && !g_state.editor_presented_logged) {
        g_state.editor_presented_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_TEXT_OVERLAY stage=editor_present"
            " primitives=%" PRIu32 " output=%" PRIu32 "x%" PRIu32
            " first_viewport=%.1f,%.1f,%.1f,%.1f\n",
            editor_drawn,
            output_width,
            output_height,
            static_cast<double>(editor_x),
            static_cast<double>(editor_y),
            static_cast<double>(editor_width),
            static_cast<double>(editor_height)
        );
        std::fflush(stderr);
    }
    if (drawn != 0u && !g_state.presented_logged) {
        g_state.presented_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_TEXT_OVERLAY stage=first_present"
            " strings=%" PRIu32 " output=%" PRIu32 "x%" PRIu32
            " guest_glyphs=suppressed_at_exact_owner\n",
            drawn,
            output_width,
            output_height
        );
        std::fflush(stderr);
    }
    g_last_draw_selected_weapon.store(
        selected_weapon_drawn,
        std::memory_order_release
    );
}

uint32_t bumble::text_overlay::last_draw_selected_weapon() {
    return g_last_draw_selected_weapon.load(std::memory_order_acquire);
}

void bumble::text_overlay::submission_complete(uint64_t submission_id) {
    std::scoped_lock lock(g_state.mutex);
    g_state.completed_submission_id = std::max(
        g_state.completed_submission_id,
        submission_id
    );
    std::erase_if(
        g_state.retired_resources,
        [](const RetiredRenderedResources& retired) {
            return retired.last_submission_id <=
                g_state.completed_submission_id;
        }
    );
}

void bumble::text_overlay::release_renderer() {
    set_renderer_ready(false);
    std::scoped_lock lock(g_state.mutex);
    g_state.slots.clear();
    g_state.retired_resources.clear();
    g_state.completed_submission_id = 0;
    g_state.menu_background_descriptor_set.reset();
    g_state.main_menu_background_descriptor_set.reset();
    g_state.menu_background_texture.reset();
    g_state.main_menu_background_texture.reset();
    g_state.menu_background_upload_buffer.reset();
    g_state.main_menu_background_upload_buffer.reset();
    g_state.menu_background_upload_failure_logged = false;
    g_state.main_menu_background_upload_failure_logged = false;
    g_state.menu_background_presented_logged = false;
    g_state.main_menu_background_presented_logged = false;
    g_state.pipeline.reset();
    g_state.pipeline_layout.reset();
    g_state.render_interface = nullptr;
    g_state.device = nullptr;
    g_state.shader_library = nullptr;
    g_state.swap_chain = nullptr;
    g_state.output_width = 0u;
    g_state.output_height = 0u;
    g_state.ready = false;
    g_state.font_ready = false;
    g_state.presented_logged = false;
    g_state.wide_panel_layout_logged = false;
}
