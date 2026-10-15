// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"

namespace Frontend {

namespace {

// Issue #69: the monitor the window (and fullscreen) goes to. BB_DISPLAY: its number in SDL's
// order (1, 2, ...; bb-gpu-capabilities --displays lists them) or a part of its name; without it
// SDL's primary display. The monitors are logged so a report says which one was taken.
SDL_DisplayID ChooseDisplay() {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    const char* wanted = std::getenv("BB_DISPLAY");
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    SDL_DisplayID chosen = 0;
    if (wanted && *wanted) {
        char* end = nullptr;
        const long number = std::strtol(wanted, &end, 10);
        if (end && *end == '\0') {
            if (number >= 1 && number <= count) {
                chosen = ids[number - 1];
            }
        } else {
            for (int i = 0; i < count && !chosen; ++i) {
                const char* name = SDL_GetDisplayName(ids[i]);
                if (name && strcasestr(name, wanted)) {
                    chosen = ids[i];
                }
            }
        }
    }
    const SDL_DisplayID display = chosen ? chosen : primary;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetDisplayName(ids[i]);
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]);
        std::printf("Display %d: %s %dx%d%s%s\n", i + 1, name ? name : "?", mode ? mode->w : 0,
                    mode ? mode->h : 0, ids[i] == primary ? " (primary)" : "",
                    ids[i] == display ? " <- the game's (BB_DISPLAY)" : "");
    }
    if (wanted && *wanted && !chosen) {
        std::printf("Display: BB_DISPLAY=%s matches none, the primary one is used\n", wanted);
    }
    SDL_free(ids);
    return display;
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    const SDL_DisplayID display = ChooseDisplay();
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::TakeMouse(double& dx, double& dy, int& up, int& down) {
    std::scoped_lock lock{mouse_mutex};
    dx = mouse_dx;
    dy = mouse_dy;
    up = wheel_up;
    down = wheel_down;
    mouse_dx = mouse_dy = 0;
    wheel_up = wheel_down = 0;
}

void WindowSDL::TakeUi(float& x, float& y, int& left, int& right, int& wheel, int& typing) {
    x = ui_x.load(std::memory_order_relaxed);
    y = ui_y.load(std::memory_order_relaxed);
    left = ui_left.exchange(0, std::memory_order_relaxed);
    right = ui_right.exchange(0, std::memory_order_relaxed);
    wheel = ui_wheel.exchange(0, std::memory_order_relaxed);
    typing = ui_typing.load(std::memory_order_relaxed);
}

// Mouse look: relative mode while the window holds the mouse (runtime_pad.c turns its motion into
// the right stick). Motion from before is dropped, so taking the mouse does not turn the camera.
void WindowSDL::CaptureMouse(bool capture) {
    if (capture == mouse_captured.load(std::memory_order_relaxed)) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(window, capture) && capture) {
        std::printf("Mouse look: relative mouse mode failed: %s\n", SDL_GetError());
        return;
    }
    {
        std::scoped_lock lock{mouse_mutex};
        mouse_dx = mouse_dy = 0;
        wheel_up = wheel_down = 0;
    }
    mouse_captured.store(capture, std::memory_order_relaxed);
    static bool told;
    if (!told) {
        told = true;
        std::printf("Mouse: the cursor shows in menus. Click a row to choose it, right click goes back.\n");
    }
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextPrompt(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            std::scoped_lock lock{mouse_mutex};
            mouse_dx += event.motion.xrel;
            mouse_dy += event.motion.yrel;
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        // F1 frees the cursor during play. A menu shows it on its own and takes it back after.
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F1 &&
            mouse_look.load(std::memory_order_relaxed) && !BbOverlay::MenuOpen()) {
            if (auto_look.load(std::memory_order_relaxed)) {
                user_freed.store(!user_freed.load(std::memory_order_relaxed), std::memory_order_relaxed);
            } else {
                CaptureMouse(!mouse_captured.load(std::memory_order_relaxed));
            }
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button == SDL_BUTTON_LEFT) ui_left.store(1, std::memory_order_relaxed);
            if (event.button.button == SDL_BUTTON_RIGHT) ui_right.store(1, std::memory_order_relaxed);
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            const float notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            if (notches != 0 && menu_pointer.load(std::memory_order_relaxed)) {
                ui_wheel.fetch_add(notches > 0 ? 1 : -1, std::memory_order_relaxed);
            } else if (notches != 0) {
                std::scoped_lock lock{mouse_mutex};
                (notches > 0 ? wheel_up : wheel_down) += 1;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            CaptureMouse(false);
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    // The port menu, the name box and mouse_look=0 want the cursor. With the menu pointer
    // hooked, a game menu does too, and play captures the mouse again when the menu closes.
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    ui_x.store(mx, std::memory_order_relaxed);
    ui_y.store(my, std::memory_order_relaxed);
    ui_typing.store(text_active ? 1 : 0, std::memory_order_relaxed);
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool menu = menu_pointer.load(std::memory_order_relaxed);
    const bool automatic = auto_look.load(std::memory_order_relaxed);
    const bool release = BbOverlay::MenuOpen() || text_active || !mouse_look.load(std::memory_order_relaxed) ||
                         (automatic && (menu || user_freed.load(std::memory_order_relaxed) || !focused));
    const bool grab = automatic && !menu && focused && mouse_look.load(std::memory_order_relaxed) &&
                      !user_freed.load(std::memory_order_relaxed) && !BbOverlay::MenuOpen() && !text_active;
    if (mouse_captured.load(std::memory_order_relaxed) && release) CaptureMouse(false);
    else if (!mouse_captured.load(std::memory_order_relaxed) && grab) CaptureMouse(true);
    UpdateCursor();
    return is_open;
}

// Hidden while looking (the pointer is the camera). Shown for menus, including fullscreen,
// so a click can confirm and the pointer can move the selection.
void WindowSDL::UpdateCursor() {
    const bool hide = mouse_captured.load(std::memory_order_relaxed) && !BbOverlay::MenuOpen();
    if (hide != cursor_hidden) {
        cursor_hidden = hide;
        hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

} // namespace Frontend
