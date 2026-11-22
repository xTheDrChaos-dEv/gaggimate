#include "SdlDriver.h"

#include <SDL.h>
#include <SD_MMC.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <display/main.h>
#include <lvgl.h>
#include <vector>

SdlDriver *SdlDriver::instance = nullptr;
float g_simScaleTarget = -1.0f;
float g_simScaleOffset = 0.0f;
static constexpr float SIM_CUP_WEIGHT = 62.4f; // g, empty dosing cup

// The LilyGo T-RGB panel the default UI targets is 480x480.
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 480;

static SDL_Window *s_window = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static SDL_Texture *s_texture = nullptr;
static SDL_Texture *s_mask = nullptr; // opaque outside the 480px circle, transparent inside

// "Bezel" colour shown outside the round panel (corners of the square window).
static constexpr Uint8 BEZEL_R = 0x28, BEZEL_G = 0x28, BEZEL_B = 0x28;

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf = nullptr;

static int s_mouseX = 0;
static int s_mouseY = 0;
static bool s_mousePressed = false;
static bool s_quit = false;

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    SDL_Rect r{area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1};
    SDL_UpdateTexture(s_texture, &r, color_p, r.w * (int)sizeof(lv_color_t));
    lv_disp_flush_ready(drv);
}

static void mouse_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    data->point.x = s_mouseX;
    data->point.y = s_mouseY;
    data->state = s_mousePressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// ---- Scripted control (automation) ------------------------------------------
// With GAGGIMATE_SIM_CONTROL=<dir> set, the simulator polls <dir>/cmd once per frame, runs every line in
// it and then deletes the file (write cmd.tmp, then rename it to cmd, so a half-written file is never read).
// This drives the real UI deterministically for scripted walkthroughs and screenshots, on any host OS:
//   tap <x> <y> [holdMs]    touch the panel at (x, y), default 150 ms
//   button <0|1> <holdMs>   hold the controller's brew (0) or steam (1) input
//   scale <grams|off|tare>  simulated Bluetooth scale: weight on the scale, disconnect, or tare
//   shot <path.bmp>         save the current frame (same output as --screenshot)
static const char *s_controlDir = nullptr;
static bool s_tapActive = false;
static Uint32 s_tapReleaseAt = 0;
static int s_buttonHeld[2] = {0, 0};
static Uint32 s_buttonReleaseAt[2] = {0, 0};

static void sim_control_poll(SdlDriver *driver) {
    if (s_controlDir == nullptr)
        return;
    const Uint32 now = SDL_GetTicks();
    if (s_tapActive && SDL_TICKS_PASSED(now, s_tapReleaseAt)) {
        s_tapActive = false;
        s_mousePressed = false;
    }
    for (int i = 0; i < 2; i++) {
        if (s_buttonHeld[i] && SDL_TICKS_PASSED(now, s_buttonReleaseAt[i])) {
            s_buttonHeld[i] = 0;
            controller.getClientController()->simulateButton((uint8_t)i, false);
        }
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/cmd", s_controlDir);
    FILE *f = fopen(path, "r");
    if (f == nullptr)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr) {
        int a = 0, b = 0, c = 150;
        float grams = 0.0f;
        char arg[400];
        if (sscanf(line, "tap %d %d %d", &a, &b, &c) >= 2) {
            s_mouseX = a;
            s_mouseY = b;
            s_mousePressed = true;
            s_tapActive = true;
            s_tapReleaseAt = now + (Uint32)c;
        } else if (sscanf(line, "button %d %d", &a, &b) == 2 && (a == 0 || a == 1)) {
            s_buttonHeld[a] = 1;
            s_buttonReleaseAt[a] = now + (Uint32)b;
            controller.getClientController()->simulateButton((uint8_t)a, true);
        } else if (sscanf(line, "scale %f", &grams) == 1) {
            g_simScaleTarget = grams;
        } else if (strncmp(line, "scale off", 9) == 0) {
            g_simScaleTarget = -1.0f;
            g_simScaleOffset = 0.0f;
        } else if (strncmp(line, "scale tare", 10) == 0) {
            g_simScaleOffset = g_simScaleTarget > 0.0f ? g_simScaleTarget : 0.0f;
        } else if (sscanf(line, "shot %399[^\r\n]", arg) == 1) {
            driver->screenshot(arg);
        }
    }
    fclose(f);
    remove(path);
}

void SdlDriver::init() {
    s_controlDir = getenv("GAGGIMATE_SIM_CONTROL");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }
    s_window =
        SDL_CreateWindow("GaggiMate Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISP_W, DISP_H, SDL_WINDOW_SHOWN);
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, DISP_W, DISP_H);

    // Build the round-display mask: the device panel is a 480px circle, so paint
    // everything outside that circle with the bezel colour and leave the inside
    // transparent. Overlaid on every frame so the square framebuffer reads as round.
    s_mask = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, DISP_W, DISP_H);
    SDL_SetTextureBlendMode(s_mask, SDL_BLENDMODE_BLEND);
    {
        std::vector<uint32_t> px(DISP_W * DISP_H);
        const double cx = DISP_W / 2.0, cy = DISP_H / 2.0, r = DISP_W / 2.0;
        const uint32_t bezel = 0xFF000000u | (BEZEL_R << 16) | (BEZEL_G << 8) | BEZEL_B;
        for (int y = 0; y < DISP_H; y++) {
            for (int x = 0; x < DISP_W; x++) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                px[y * DISP_W + x] = (dx * dx + dy * dy <= r * r) ? 0x00000000u : bezel;
            }
        }
        SDL_UpdateTexture(s_mask, nullptr, px.data(), DISP_W * (int)sizeof(uint32_t));
    }

    lv_init();
    s_buf = (lv_color_t *)malloc(DISP_W * DISP_H * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&s_draw_buf, s_buf, nullptr, DISP_W * DISP_H);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISP_W;
    disp_drv.ver_res = DISP_H;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_read;
    lv_indev_drv_register(&indev_drv);
}

void SdlDriver::pumpAndRender() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            s_quit = true;
            break;
        case SDL_MOUSEMOTION:
            if (s_tapActive) // a scripted tap owns the touch point until it releases
                break;
            s_mouseX = e.motion.x;
            s_mouseY = e.motion.y;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                s_mousePressed = true;
                s_mouseX = e.button.x;
                s_mouseY = e.button.y;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT)
                s_mousePressed = false;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:                                       // keys 1/2 act as the brew/steam inputs of the controller board
            if (e.type == SDL_KEYDOWN && e.key.repeat == 0) { // simulated Bluetooth scale (see SdlDriver.h)
                switch (e.key.keysym.sym) {
                case SDLK_0:
                    g_simScaleTarget = -1.0f;
                    g_simScaleOffset = 0.0f;
                    break;
                case SDLK_t:
                    g_simScaleOffset = g_simScaleTarget > 0.0f ? g_simScaleTarget : 0.0f;
                    break;
                case SDLK_3:
                    g_simScaleTarget = 0.0f;
                    break;
                case SDLK_4:
                    g_simScaleTarget = SIM_CUP_WEIGHT;
                    break;
                case SDLK_5:
                    g_simScaleTarget = SIM_CUP_WEIGHT + 18.0f;
                    break;
                case SDLK_6:
                    g_simScaleTarget = SIM_CUP_WEIGHT + 20.5f;
                    break;
                default:
                    break;
                }
            }
            if (e.key.repeat == 0 && (e.key.keysym.sym == SDLK_1 || e.key.keysym.sym == SDLK_2))
                controller.getClientController()->simulateButton(e.key.keysym.sym == SDLK_1 ? 0 : 1, e.type == SDL_KEYDOWN);
            break;
        default:
            break;
        }
    }
    sim_control_poll(this);
    SDL_SetRenderDrawColor(s_renderer, BEZEL_R, BEZEL_G, BEZEL_B, 0xFF);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
    SDL_RenderCopy(s_renderer, s_mask, nullptr, nullptr); // round off the corners
    SDL_RenderPresent(s_renderer);
}

bool SdlDriver::installSDCard() { return SD_MMC.begin("/sdcard", true, false); }

bool SdlDriver::shouldQuit() const { return s_quit; }

void SdlDriver::screenshot(const char *path) {
    SDL_SetRenderDrawColor(s_renderer, BEZEL_R, BEZEL_G, BEZEL_B, 0xFF);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
    SDL_RenderCopy(s_renderer, s_mask, nullptr, nullptr); // round off the corners
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, DISP_W, DISP_H, 16, SDL_PIXELFORMAT_RGB565);
    if (surface) {
        SDL_RenderReadPixels(s_renderer, nullptr, SDL_PIXELFORMAT_RGB565, surface->pixels, surface->pitch);
        SDL_SaveBMP(surface, path);
        SDL_FreeSurface(surface);
    }
    SDL_RenderPresent(s_renderer);
}
