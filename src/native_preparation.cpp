#include "native_preparation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <SDL_syswm.h>
#endif
#include <memory>
#include <vector>
#include <SDL.h>
#include <stb_truetype.h>
#include "bumble_text_font.h"

namespace bumble::first_run {
void PreparationProgress::check() const {
    if (cancelled.load(std::memory_order_acquire)) throw PreparationCancelled();
}

void PreparationProgress::report(std::string stage, uint32_t completed, uint32_t total) {
    check();
    std::lock_guard lock(mutex);
    value = {std::move(stage), completed, total};
}

PreparationProgress::Snapshot PreparationProgress::snapshot() {
    std::lock_guard lock(mutex);
    return value;
}

namespace {
struct VideoScope {
    VideoScope() { if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError()); }
    ~VideoScope() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
};
struct PreparationWindow {
    VideoScope video;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{nullptr, SDL_DestroyWindow};
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> canvas{nullptr, SDL_FreeSurface};
    std::array<stbtt_bakedchar, 96> glyphs{};
    std::vector<unsigned char> atlas = std::vector<unsigned char>(512 * 256);
    explicit PreparationWindow(int height) {
        window.reset(SDL_CreateWindow("Bumble", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            640, height, SDL_WINDOW_SHOWN));
        if (!window) throw std::runtime_error(SDL_GetError());
        canvas.reset(SDL_CreateRGBSurfaceWithFormat(0, 640, height, 32, SDL_PIXELFORMAT_RGBA32));
        if (!canvas) throw std::runtime_error(SDL_GetError());
        if (stbtt_BakeFontBitmap(reinterpret_cast<const unsigned char*>(BumbleTextFont),
                0, 23, atlas.data(), 512, 256, 32, int(glyphs.size()), glyphs.data()) <= 0)
            throw std::runtime_error("Cannot load preparation font");
    }
    void text(const std::string& value, int left, int baseline) {
        float x = float(left), y = float(baseline);
        for (unsigned char c : value) {
            if (c < 32 || c >= 128) continue;
            stbtt_aligned_quad quad;
            stbtt_GetBakedQuad(glyphs.data(), 512, 256, c - 32, &x, &y, &quad, 1);
            const auto& glyph = glyphs[c - 32];
            for (int row = 0; row < glyph.y1 - glyph.y0; ++row)
                for (int col = 0; col < glyph.x1 - glyph.x0; ++col) {
                    const int px = int(quad.x0) + col, py = int(quad.y0) + row;
                    if (px < 0 || px >= canvas->w || py < 0 || py >= canvas->h) continue;
                    const unsigned alpha = atlas[(glyph.y0 + row) * 512 + glyph.x0 + col];
                    auto* pixel = static_cast<uint8_t*>(canvas->pixels) + py * canvas->pitch + px * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        pixel[channel] = uint8_t((pixel[channel] * (255 - alpha) + 240 * alpha) / 255);
                }
        }
    }

    void present() {
        if (auto* surface = SDL_GetWindowSurface(window.get())) {
            SDL_BlitSurface(canvas.get(), nullptr, surface, nullptr);
            SDL_UpdateWindowSurface(window.get());
        }
    }
};
bool closing(const SDL_Event& event, Uint32 id) {
    return event.type == SDL_QUIT ||
        (event.type == SDL_WINDOWEVENT && event.window.windowID == id && event.window.event == SDL_WINDOWEVENT_CLOSE) ||
        (event.type == SDL_KEYDOWN && event.key.windowID == id && event.key.keysym.sym == SDLK_ESCAPE);
}
}

void run_preparation(const std::function<void(PreparationProgress&)>& work, bool checking) {
    PreparationWindow ui(240);
    auto& window = ui.window;
    auto& canvas = ui.canvas;
    const auto text = [&](const std::string& value, int x, int y) { ui.text(value, x, y); };
    PreparationProgress progress;
    progress.report(checking ? "Checking textures" : "Preparing menus");
    auto pending = std::async(std::launch::async, [&]() { work(progress); });
    std::string lastStage;
    uint32_t lastCompleted = UINT32_MAX, lastTotal = UINT32_MAX;
    const auto started = std::chrono::steady_clock::now();
    uint64_t lastSecond = UINT64_MAX;
    bool redraw = true;
    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.windowID == SDL_GetWindowID(window.get()) &&
                    event.window.event == SDL_WINDOWEVENT_CLOSE) ||
                (event.type == SDL_KEYDOWN && event.key.windowID == SDL_GetWindowID(window.get()) &&
                    event.key.keysym.sym == SDLK_ESCAPE)) progress.cancelled.store(true);
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) redraw = true;
        }
        auto state = progress.snapshot();
        const auto seconds = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count());
        if (progress.cancelled.load()) state = {"Cancelling...", 0, 0};
        if (redraw || seconds != lastSecond || state.stage != lastStage || state.completed != lastCompleted || state.total != lastTotal) {
            SDL_FillRect(canvas.get(), nullptr, SDL_MapRGB(canvas->format, 28, 30, 36));
            text(checking ? "Checking game textures" : "Preparing game textures", 30, 47);
            text(checking ? "Validating cached files." : "This runs once. Please keep the game open.", 30, 82);
            text(state.stage, 30, 120);
            SDL_Rect track{30, 139, 580, 20};
            SDL_FillRect(canvas.get(), &track, SDL_MapRGB(canvas->format, 64, 67, 76));
            if (state.total) {
                SDL_Rect fill = track;
                fill.w = int(uint64_t(std::min(state.completed, state.total)) * track.w / state.total);
                SDL_FillRect(canvas.get(), &fill, SDL_MapRGB(canvas->format, 238, 178, 38));
                text(std::to_string(state.completed) + " / " + std::to_string(state.total), 30, 193);
            }
            text(std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s elapsed", 360, 193);
            text("Close this window or press Escape to cancel.", 30, 222);
            auto* surface = SDL_GetWindowSurface(window.get());
            if (surface) {
                SDL_BlitSurface(canvas.get(), nullptr, surface, nullptr);
                SDL_UpdateWindowSurface(window.get());
            }
            lastStage = state.stage; lastCompleted = state.completed; lastTotal = state.total; redraw = false;
            lastSecond = seconds;
        }
        if (pending.wait_for(std::chrono::milliseconds(33)) == std::future_status::ready) break;
    }
    pending.get();
    progress.check();
}
TextureChoice choose_enhanced_textures() {
    PreparationWindow ui(330);
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) throw std::runtime_error(SDL_GetError());
    struct Controllers {
        std::vector<std::unique_ptr<SDL_GameController, decltype(&SDL_GameControllerClose)>> open;
        ~Controllers() { open.clear(); SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER); }
        void add(int index) {
            const auto id = SDL_JoystickGetDeviceInstanceID(index);
            for (const auto& c : open)
                if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c.get())) == id) return;
            if (SDL_IsGameController(index))
                if (auto* controller = SDL_GameControllerOpen(index)) open.emplace_back(controller, SDL_GameControllerClose);
        }
    } controllers;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) controllers.add(i);
#if defined(_WIN32)
    const char* validation = std::getenv("BUMBLE_RT64_VALIDATION_SYNTHETIC_INPUT");
    const bool synthetic = validation && std::strcmp(validation, "1") == 0;
    const Uint8 previousSyswm = SDL_EventState(SDL_SYSWMEVENT, SDL_QUERY);
    if (synthetic) SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    struct SyswmScope { Uint8 previous; ~SyswmScope() { SDL_EventState(SDL_SYSWMEVENT, previous); } } syswm{previousSyswm};
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(ui.window.get(), &info);
#endif
    TextureChoice choice;
    int focus = 2;
    std::array<bool, 2> axisHeld{};
    bool redraw = true;
    const SDL_Rect boxes[] = {{30, 176, 580, 38}, {30, 234, 280, 44}, {330, 234, 280, 44}};
    for (;;) {
        if (redraw) {
            SDL_FillRect(ui.canvas.get(), nullptr, SDL_MapRGB(ui.canvas->format, 28, 30, 36));
            ui.text("Generate enhanced textures?", 30, 43);
            ui.text("Optional: about 120 MB of additional disk space.", 30, 83);
            ui.text("Preparation takes a few minutes and runs once.", 30, 115);
            ui.text("Original and Modern modes work without them.", 30, 147);
            for (int i = 0; i < 3; ++i)
                SDL_FillRect(ui.canvas.get(), &boxes[i], SDL_MapRGB(ui.canvas->format,
                    focus == i ? 120 : 50, focus == i ? 91 : 53, focus == i ? 29 : 62));
            SDL_Rect checkbox{42, 183, 24, 24};
            SDL_FillRect(ui.canvas.get(), &checkbox, SDL_MapRGB(ui.canvas->format, 220, 223, 230));
            SDL_Rect inset{44, 185, 20, 20};
            SDL_FillRect(ui.canvas.get(), &inset, SDL_MapRGB(ui.canvas->format, 28, 30, 36));
            if (choice.remember) {
                const Uint32 ink = SDL_MapRGB(ui.canvas->format, 238, 178, 38);
                for (int x = 0; x < 6; ++x) {
                    SDL_Rect stroke{47 + x, 193 + x, 3, 3};
                    SDL_FillRect(ui.canvas.get(), &stroke, ink);
                }
                for (int x = 0; x < 10; ++x) {
                    SDL_Rect stroke{52 + x, 198 - x, 3, 3};
                    SDL_FillRect(ui.canvas.get(), &stroke, ink);
                }
            }
            ui.text("Don't ask again", 78, 201);
            ui.text("Generate", 113, 263);
            ui.text("Not now", 420, 263);
            ui.text("Arrows / Tab: move   Enter / Space: choose", 30, 311);
            ui.present();
            redraw = false;
        }
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 50)) continue;
        if (closing(event, SDL_GetWindowID(ui.window.get()))) throw PreparationCancelled();
        SDL_Keycode key = SDLK_UNKNOWN;
        bool activate = false, released = event.type == SDL_KEYUP;
        if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
            event.key.windowID == SDL_GetWindowID(ui.window.get())) key = event.key.keysym.sym;
#if defined(_WIN32)
        if (synthetic && event.type == SDL_SYSWMEVENT && event.syswm.msg &&
            event.syswm.msg->subsystem == SDL_SYSWM_WINDOWS &&
            event.syswm.msg->msg.win.hwnd == info.info.win.window &&
            event.syswm.msg->msg.win.msg == WM_APP + 0x43u) {
            released = event.syswm.msg->msg.win.lParam == 0;
            switch (event.syswm.msg->msg.win.wParam) {
                case VK_UP: key = SDLK_UP; break;
                case VK_DOWN: key = SDLK_DOWN; break;
                case VK_LEFT: key = SDLK_LEFT; break;
                case VK_RIGHT: key = SDLK_RIGHT; break;
                case VK_TAB: key = SDLK_TAB; break;
                case VK_RETURN: key = SDLK_RETURN; break;
                case VK_SPACE: key = SDLK_SPACE; break;
                case VK_ESCAPE: throw PreparationCancelled();
            }
        }
#endif
        if (event.type == SDL_CONTROLLERDEVICEADDED) controllers.add(event.cdevice.which);
        if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            const auto id = event.cdevice.which;
            std::erase_if(controllers.open, [&](const auto& c) {
                return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c.get())) == id;
            });
        }
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: key = SDLK_UP; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key = SDLK_LEFT; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key = SDLK_DOWN; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key = SDLK_RIGHT; break;
                case SDL_CONTROLLER_BUTTON_B: throw PreparationCancelled();
            }
        }
        if (event.type == SDL_CONTROLLERBUTTONUP && event.cbutton.button == SDL_CONTROLLER_BUTTON_A) activate = true;
        if (event.type == SDL_CONTROLLERAXISMOTION &&
            (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)) {
            const bool held = std::abs(int(event.caxis.value)) > 16000;
            auto& previous = axisHeld[event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? 0 : 1];
            if (held && !previous) key = event.caxis.value < 0 ? SDLK_UP : SDLK_DOWN;
            previous = held;
        }
        if (event.type == SDL_MOUSEBUTTONUP && event.button.windowID == SDL_GetWindowID(ui.window.get()) &&
            event.button.button == SDL_BUTTON_LEFT) {
            const SDL_Point point{event.button.x, event.button.y};
            for (int i = 0; i < 3; ++i) if (SDL_PointInRect(&point, &boxes[i])) { focus = i; activate = true; }
        }
        if (!released && (key == SDLK_UP || key == SDLK_LEFT)) { focus = (focus + 2) % 3; redraw = true; }
        if (!released && (key == SDLK_DOWN || key == SDLK_RIGHT || key == SDLK_TAB)) { focus = (focus + 1) % 3; redraw = true; }
        activate |= released && (key == SDLK_RETURN || key == SDLK_SPACE);
        if (activate) {
            if (focus == 0) { choice.remember = !choice.remember; redraw = true; }
            else { choice.generate = focus == 1; return choice; }
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) redraw = true;
    }
}

}
