#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bumble::text_overlay {

enum class TextKind : uint8_t {
    Script,
    Briefing,
    GameplayHud,
    Credits,
    Menu,
    HighScores,
    Editor,
    Completion,
    CombatHud
};

enum class OverlayPrimitive : uint8_t {
    Text,
    Panel,
    HealthHoneycomb,
    MenuBackdrop,
    MainMenuBackdrop,
    CompletionBackdrop
};

enum class HorizontalAnchor : uint8_t {
    Authored,
    Left,
    Center,
    CenterText,
    Right,
    RightInset,
    Panel,
    RightTextInset
};

enum class VerticalAnchor : uint8_t {
    Authored,
    Top,
    Center,
    Bottom
};

struct Observation {
    TextKind kind = TextKind::Script;
    OverlayPrimitive primitive = OverlayPrimitive::Text;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    HorizontalAnchor horizontal_anchor = HorizontalAnchor::Authored;
    VerticalAnchor vertical_anchor = VerticalAnchor::Authored;
    float screen_scale = 1.0f;
    uint32_t face_rgba = 0xFFFFFFFFu;
    uint32_t outline_rgba = 0x000000FFu;
    float fill_fraction = 0.0f;
    uint32_t cell_count = 0u;
    std::string text;
    bool panel_reveal = false;
    uint32_t visible_characters = UINT32_MAX;
    uint64_t stable_slot = 0;
    uint64_t revision = 0;
};

bool observe(
    TextKind kind,
    uint32_t x,
    uint32_t y,
    const char* text,
    HorizontalAnchor horizontal_anchor = HorizontalAnchor::Authored,
    VerticalAnchor vertical_anchor = VerticalAnchor::Authored,
    float screen_scale = 1.0f,
    uint32_t face_rgba = 0xFFFFFFFFu,
    uint32_t outline_rgba = 0x000000FFu,
    uint64_t stable_slot = 0
);
bool observe_menu_backdrop(
    uint32_t rgba = 0x162A3DDBu,
    TextKind kind = TextKind::Menu,
    bool main_menu = false
);
bool observe_completion_backdrop();
bool observe_briefing(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const std::string& text, uint32_t visible_characters);
bool observe_panel(
    TextKind kind,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t rgba,
    HorizontalAnchor horizontal_anchor = HorizontalAnchor::Authored,
    VerticalAnchor vertical_anchor = VerticalAnchor::Authored,
    uint64_t stable_slot = 0
);
bool observe_health_honeycomb(
    uint32_t x,
    uint32_t y,
    float fill_fraction,
    uint32_t cell_count
);
std::vector<Observation> active_observations();

// Display the last submitted text until the next graphics task atomically replaces it.
void begin_frame_observations();
void commit_frame_observations();

void set_renderer_ready(bool ready);
bool renderer_ready();
void begin_menu_observations();
bool commit_menu_observations();
void cancel_menu_observations();
void clear_kind(TextKind kind);
void clear();

} // namespace bumble::text_overlay
