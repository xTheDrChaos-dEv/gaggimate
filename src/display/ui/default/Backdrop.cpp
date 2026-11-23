#include "Backdrop.h"
#include "BackdropBeans.h"
#include "BackdropFloral.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>
#include <display/ui/default/eez/screens.h>
#include <esp32-hal-psram.h>

namespace {

constexpr int SIZE = 480;
constexpr float HALF = SIZE / 2.0f;
constexpr int MAX_TILE = backdrop_floral::TILE; // the larger of the two pattern tiles
constexpr unsigned long DRIFT_STEP_MS = 120; // one pixel per step: about 8 px a second
constexpr float PI_F = 3.14159265f;

uint16_t *pixels = nullptr; // what the screens show: RGB565, SIZE x SIZE, PSRAM
uint16_t *tile = nullptr;   // the pattern in the theme's colours: RGB565, tileSize x tileSize, PSRAM
int tileSize = backdrop_beans::TILE;
int tilePattern = -1;
lv_img_dsc_t image = {};
int renderedTheme = -1;
int renderedStyle = -1;
int tileTheme = -1;
int driftX = 0; // where in the tile the top-left corner of the picture starts
int driftY = 0;
unsigned long lastDriftStep = 0;

struct Rgb {
    float r, g, b;
};

Rgb fromHex(uint32_t color) {
    return {static_cast<float>((color >> 16) & 0xff), static_cast<float>((color >> 8) & 0xff), static_cast<float>(color & 0xff)};
}

Rgb mix(const Rgb &a, const Rgb &b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

Rgb scale(const Rgb &c, float k) { return {c.r * k, c.g * k, c.b * k}; }

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float smoothstep(float edge0, float edge1, float v) {
    const float t = clamp01((v - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

uint32_t hash(uint32_t x, uint32_t y) {
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float lattice(int x, int y) { return static_cast<float>(hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y)) & 0xffff) / 65535.0f; }

// Smooth value noise in 0..1.
float noise(float x, float y) {
    const int xi = static_cast<int>(floorf(x));
    const int yi = static_cast<int>(floorf(y));
    float fx = x - static_cast<float>(xi);
    float fy = y - static_cast<float>(yi);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float top = lattice(xi, yi) + (lattice(xi + 1, yi) - lattice(xi, yi)) * fx;
    const float bottom = lattice(xi, yi + 1) + (lattice(xi + 1, yi + 1) - lattice(xi, yi + 1)) * fx;
    return top + (bottom - top) * fy;
}

float glow(float x, float y, float cx, float cy, float sigma) {
    const float dx = x - cx;
    const float dy = y - cy;
    return expf(-(dx * dx + dy * dy) / (2.0f * sigma * sigma));
}

// Dark surfaces only have a handful of RGB565 levels, so a smooth gradient would show as bands. An ordered dither
// spreads the rounding error over a 4x4 cell and hides them. Goes through lv_color_make so the byte order always
// matches what LVGL was built with.
uint16_t pack(const Rgb &c, int x, int y) {
    static const float BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const float d = BAYER[(y & 3) * 4 + (x & 3)] / 16.0f;
    int r = static_cast<int>(clamp01(c.r / 255.0f) * 31.0f + d);
    int g = static_cast<int>(clamp01(c.g / 255.0f) * 63.0f + d);
    int b = static_cast<int>(clamp01(c.b / 255.0f) * 31.0f + d);
    r = r > 31 ? 31 : r;
    g = g > 63 ? 63 : g;
    b = b > 31 ? 31 : b;
    return lv_color_make(static_cast<uint8_t>(r << 3), static_cast<uint8_t>(g << 2), static_cast<uint8_t>(b << 3)).full;
}

struct Palette {
    Rgb background, text, accent, aux;
};

Palette paletteOf(int theme) {
    return {fromHex(theme_colors[theme][1]), fromHex(theme_colors[theme][0]), fromHex(theme_colors[theme][2]),
            fromHex(theme_colors[theme][4])};
}

// Dark (Oxblood): a warm glow low in the dial and fine swirls, like the surface of an espresso.
Rgb crema(float x, float y, const Palette &p) {
    const float dx = x - HALF;
    const float dy = y - HALF;
    const float r = sqrtf(dx * dx + dy * dy);
    Rgb c = mix(p.background, p.accent, 0.13f * glow(x, y, HALF, HALF + 90.0f, 170.0f));
    const float swirl = sinf(r * 0.085f + 3.0f * atan2f(dy, dx) + 5.0f * noise(x * 0.011f, y * 0.011f));
    c = mix(c, p.accent, 0.075f * smoothstep(0.90f, 1.0f, swirl));
    return scale(c, 1.0f - 0.30f * (r / HALF) * (r / HALF));
}

// Olive: contour lines, like a terrain map.
Rgb contours(float x, float y, const Palette &p) {
    const float dx = x - HALF;
    const float dy = y - HALF;
    const float rn = sqrtf(dx * dx + dy * dy) / HALF;
    const float field = noise(x * 0.0105f, y * 0.0105f) * 0.68f + noise(x * 0.026f + 19.0f, y * 0.026f + 7.0f) * 0.32f;
    const float level = field * 10.0f;
    const float f = level - floorf(level);
    const float line = 1.0f - smoothstep(0.0f, 0.07f, f < 0.5f ? f : 1.0f - f);
    Rgb c = mix(p.background, p.text, 0.05f * (1.0f - y / SIZE)); // a little lighter towards the top
    c = mix(c, p.accent, 0.13f * line + 0.05f * field);
    return scale(c, 1.0f - 0.26f * rn * rn);
}

// Purple: two soft clouds of colour and a scattering of faint stars.
Rgb nightSky(float x, float y, const Palette &p) {
    const float dx = x - HALF;
    const float dy = y - HALF;
    const float rn = sqrtf(dx * dx + dy * dy) / HALF;
    Rgb c = mix(p.background, p.accent, 0.17f * glow(x, y, 150.0f, 165.0f, 150.0f));
    c = mix(c, p.aux, 0.12f * glow(x, y, 345.0f, 335.0f, 165.0f));
    c = mix(c, p.accent, 0.04f * noise(x * 0.02f, y * 0.02f));
    c = scale(c, 1.0f - 0.28f * rn * rn);
    const uint32_t h = hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    if ((h & 0x1ff) == 7) { // roughly one pixel in 500
        c = mix(c, p.text, 0.20f + 0.50f * static_cast<float>((h >> 9) & 0xff) / 255.0f);
    }
    return c;
}

// Light: paper with faint rings and a gently darker rim.
Rgb paper(float x, float y, const Palette &p) {
    const float dx = x - HALF;
    const float dy = y - HALF;
    const float r = sqrtf(dx * dx + dy * dy);
    const float rn = r / HALF;
    Rgb c = mix(p.background, p.accent, 0.055f * smoothstep(0.94f, 1.0f, sinf(r * 0.105f + 2.0f * noise(x * 0.01f, y * 0.01f))));
    c = mix(c, p.text, 0.07f * rn * rn);
    const float grain = (static_cast<float>(hash(static_cast<uint32_t>(x) + 91u, static_cast<uint32_t>(y)) & 0xff) / 255.0f - 0.5f) * 5.0f;
    return {c.r + grain, c.g + grain, c.b + grain};
}

// AMOLED: stays black (that is the point of the theme) with thin rings in the accent colour.
Rgb rings(float x, float y, const Palette &p) {
    const float dx = x - HALF;
    const float dy = y - HALF;
    const float r = sqrtf(dx * dx + dy * dy);
    const float line = smoothstep(0.965f, 1.0f, sinf(r * 0.105f + 3.0f * noise(x * 0.009f, y * 0.009f)));
    return mix(p.background, p.accent, 0.11f * line + 0.035f * glow(x, y, HALF, HALF + 110.0f, 150.0f));
}

void drawArtwork(int theme) {
    const Palette palette = paletteOf(theme);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            Rgb c;
            switch (theme) {
            case THEME_ID_LIGHT:
                c = paper(fx, fy, palette);
                break;
            case THEME_ID_AMOLED_DARK:
                c = rings(fx, fy, palette);
                break;
            case THEME_ID_OLIVE:
                c = contours(fx, fy, palette);
                break;
            case THEME_ID_PURPLE:
                c = nightSky(fx, fy, palette);
                break;
            default:
                c = crema(fx, fy, palette);
                break;
            }
            pixels[y * SIZE + x] = pack(c, x, y);
        }
    }
}

// How strongly one bean covers a point given in the bean's own axes (u along it, v across): 1 on its outline or
// its centre crease, 0 away from them, in between on the soft edge of the line.
float beanCoverage(float u, float v, int mirror) {
    using namespace backdrop_beans;
    float coverage = 0.0f;
    const float nu = u / HALF_LENGTH;
    const float nv = v / HALF_WIDTH;
    const float f = sqrtf(nu * nu + nv * nv);
    if (f > 0.05f) {
        // Distance to the ellipse, to first order: how far off the outline, over how fast that changes here.
        const float gu = u / (HALF_LENGTH * HALF_LENGTH);
        const float gv = v / (HALF_WIDTH * HALF_WIDTH);
        const float distance = fabsf(f - 1.0f) * f / sqrtf(gu * gu + gv * gv);
        coverage = clamp01(1.5f - distance); // a line about 2 px wide
    }
    // The crease: one gentle S along the bean.
    const float length = CREASE_HALF_LENGTH * HALF_LENGTH;
    const float swing = static_cast<float>(mirror) * CREASE_SWING * HALF_WIDTH;
    float distance;
    if (fabsf(u) <= length) {
        const float phase = PI_F * u / length;
        const float slope = swing * PI_F / length * cosf(phase);
        distance = fabsf(v - swing * sinf(phase)) / sqrtf(1.0f + slope * slope);
    } else {
        const float past = fabsf(u) - length;
        distance = sqrtf(past * past + v * v);
    }
    const float crease = clamp01(1.3f - distance); // slightly finer than the outline
    return crease > coverage ? crease : coverage;
}

enum Pattern { PATTERN_NONE = 0, PATTERN_BEANS = 1, PATTERN_FLORAL = 2 };

// How much of the theme's accent colour goes into the lines, toned down so that text on top stays easy to read.
// Light paper needs less for the same visible weight; pure black (AMOLED) a little less as well.
float lineStrength(int theme) { return theme == THEME_ID_LIGHT ? 0.30f : (theme == THEME_ID_AMOLED_DARK ? 0.34f : 0.40f); }

// The bean tile: the theme's background colour with the outlines in its accent colour. Wraps on all four edges.
bool paintBeans(int theme) {
    constexpr int TILE = backdrop_beans::TILE;
    auto *coverage = static_cast<uint8_t *>(ps_malloc(TILE * TILE));
    if (coverage == nullptr) {
        return false;
    }
    memset(coverage, 0, TILE * TILE);
    const int reach = static_cast<int>(backdrop_beans::HALF_LENGTH) + 3;
    for (const backdrop_beans::Bean &bean : backdrop_beans::BEANS) {
        const float radians = bean.degrees * PI_F / 180.0f;
        const float cosA = cosf(radians);
        const float sinA = sinf(radians);
        const int cx = static_cast<int>(bean.x);
        const int cy = static_cast<int>(bean.y);
        for (int y = cy - reach; y <= cy + reach; y++) {
            for (int x = cx - reach; x <= cx + reach; x++) {
                const float dx = static_cast<float>(x) + 0.5f - bean.x;
                const float dy = static_cast<float>(y) + 0.5f - bean.y;
                const float amount = beanCoverage(dx * cosA + dy * sinA, -dx * sinA + dy * cosA, bean.mirror);
                if (amount <= 0.0f) {
                    continue;
                }
                uint8_t &cell = coverage[((y % TILE + TILE) % TILE) * TILE + (x % TILE + TILE) % TILE];
                const auto value = static_cast<uint8_t>(amount * 255.0f);
                if (value > cell) {
                    cell = value;
                }
            }
        }
    }
    const Palette palette = paletteOf(theme);
    const Rgb stroke = mix(palette.background, palette.accent, lineStrength(theme));
    const uint16_t flat = lv_color_hex(theme_colors[theme][1]).full; // exactly the colour the screens use
    for (int i = 0; i < TILE * TILE; i++) {
        if (coverage[i] == 0) {
            tile[i] = flat;
        } else {
            const Rgb c = mix(palette.background, stroke, static_cast<float>(coverage[i]) / 255.0f);
            tile[i] = lv_color_make(static_cast<uint8_t>(c.r), static_cast<uint8_t>(c.g), static_cast<uint8_t>(c.b)).full;
        }
    }
    free(coverage);
    return true;
}

// The floral tile: petals and leaves in a faint tint of the accent colour, their outlines in a stronger one.
void paintFloral(int theme) {
    constexpr int TILE = backdrop_floral::TILE;
    const Palette palette = paletteOf(theme);
    const Rgb stroke = mix(palette.background, palette.accent, lineStrength(theme));
    const Rgb fill = mix(palette.background, palette.accent, theme == THEME_ID_LIGHT ? 0.07f : 0.10f);
    const uint16_t flat = lv_color_hex(theme_colors[theme][1]).full;
    for (int i = 0; i < TILE * TILE; i++) {
        const uint8_t cell = backdrop_floral::COVERAGE[i];
        if (cell == 0) {
            tile[i] = flat;
            continue;
        }
        Rgb c = mix(palette.background, fill, static_cast<float>(cell & 0x0f) / 15.0f);
        c = mix(c, stroke, static_cast<float>(cell >> 4) / 15.0f);
        tile[i] = lv_color_make(static_cast<uint8_t>(c.r), static_cast<uint8_t>(c.g), static_cast<uint8_t>(c.b)).full;
    }
}

// Makes the tile hold a pattern in a theme's colours. False when there is no memory for it.
bool buildTile(int theme, int pattern) {
    if (tile != nullptr && tileTheme == theme && tilePattern == pattern) {
        return true;
    }
    if (tile == nullptr) {
        tile = static_cast<uint16_t *>(ps_malloc(MAX_TILE * MAX_TILE * sizeof(uint16_t)));
        if (tile == nullptr) {
            return false;
        }
    }
    tilePattern = -1; // not valid while it is being repainted
    if (pattern == PATTERN_FLORAL) {
        paintFloral(theme);
        tileSize = backdrop_floral::TILE;
    } else {
        if (!paintBeans(theme)) {
            return false;
        }
        tileSize = backdrop_beans::TILE;
    }
    tileTheme = theme;
    tilePattern = pattern;
    driftX %= tileSize;
    driftY %= tileSize;
    return true;
}

// Fills the picture with the tile, starting at (driftX, driftY) in the tile. Whole runs are copied, so one step of
// a drifting background costs a few milliseconds.
void drawTile() {
    for (int y = 0; y < SIZE; y++) {
        const uint16_t *source = tile + ((y + driftY) % tileSize) * tileSize;
        uint16_t *target = pixels + y * SIZE;
        int x = 0;
        int from = driftX % tileSize;
        while (x < SIZE) {
            int run = tileSize - from;
            if (run > SIZE - x) {
                run = SIZE - x;
            }
            memcpy(target + x, source + from, static_cast<size_t>(run) * sizeof(uint16_t));
            x += run;
            from = 0;
        }
    }
}

int patternOf(int style) {
    switch (style) {
    case backdrop::STYLE_BEANS:
    case backdrop::STYLE_BEANS_LEFT:
    case backdrop::STYLE_BEANS_DIAGONAL:
        return PATTERN_BEANS;
    case backdrop::STYLE_FLORAL:
    case backdrop::STYLE_FLORAL_LEFT:
    case backdrop::STYLE_FLORAL_DIAGONAL:
        return PATTERN_FLORAL;
    default:
        return PATTERN_NONE;
    }
}

bool driftsLeft(int style) { return style == backdrop::STYLE_BEANS_LEFT || style == backdrop::STYLE_FLORAL_LEFT; }

bool driftsDiagonally(int style) { return style == backdrop::STYLE_BEANS_DIAGONAL || style == backdrop::STYLE_FLORAL_DIAGONAL; }

} // namespace

namespace backdrop {

void render(int theme, int style) {
    if (theme < 0 || theme > THEME_ID_PURPLE) {
        theme = THEME_ID_DARK;
    }
    if (style < 0 || style >= STYLE_COUNT) {
        style = STYLE_BEANS_DIAGONAL;
    }
    if (theme == renderedTheme && style == renderedStyle) {
        return;
    }
    renderedTheme = theme;
    renderedStyle = style;
    if (style == STYLE_SOLID) {
        return; // nothing to draw: applyTo() takes the picture off the screens
    }
    if (pixels == nullptr) {
        pixels = static_cast<uint16_t *>(ps_malloc(SIZE * SIZE * sizeof(uint16_t)));
        if (pixels == nullptr) {
            return; // no memory: the screens keep their flat theme colour
        }
        image.header.cf = LV_IMG_CF_TRUE_COLOR;
        image.header.always_zero = 0;
        image.header.reserved = 0;
        image.header.w = SIZE;
        image.header.h = SIZE;
        image.data_size = SIZE * SIZE * sizeof(uint16_t);
        image.data = reinterpret_cast<const uint8_t *>(pixels);
    }
    const int pattern = patternOf(style);
    if (pattern != PATTERN_NONE && buildTile(theme, pattern)) {
        if (!driftsLeft(style) && !driftsDiagonally(style)) {
            driftX = 0;
            driftY = 0;
        }
        drawTile();
    } else {
        drawArtwork(theme);
    }
    lv_img_cache_invalidate_src(&image);
}

void applyTo(lv_obj_t *screen) {
    if (screen == nullptr) {
        return;
    }
    if (renderedStyle == STYLE_SOLID || pixels == nullptr) {
        if (lv_obj_get_style_bg_img_src(screen, LV_PART_MAIN) == &image) {
            lv_obj_set_style_bg_img_src(screen, nullptr, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_invalidate(screen);
        }
        return;
    }
    lv_obj_set_style_bg_img_src(screen, &image, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_invalidate(screen);
}

static bool isThemeBackground(lv_color_t color) {
    for (int theme = 0; theme <= THEME_ID_PURPLE; theme++) {
        if (lv_color_hex(theme_colors[theme][1]).full == color.full) {
            return true;
        }
    }
    return false;
}

static void clearFlatPanelsIn(lv_obj_t *parent) {
    const uint32_t count = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        // Any theme's background counts: a screen built before the theme was applied still has the previous
        // theme's colour on its disc at this point.
        if (lv_obj_get_width(child) >= 300 && lv_obj_get_height(child) >= 300 &&
            lv_obj_get_style_bg_opa(child, LV_PART_MAIN) == LV_OPA_COVER &&
            isThemeBackground(lv_obj_get_style_bg_color(child, LV_PART_MAIN))) {
            lv_obj_set_style_bg_opa(child, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        clearFlatPanelsIn(child);
    }
}

void clearFlatPanels(lv_obj_t *screen) {
    if (screen == nullptr || pixels == nullptr || renderedStyle == STYLE_SOLID) {
        return;
    }
    // A screen that was only just built has no sizes yet (they are worked out on the next redraw), and the
    // size is how the disc is recognised.
    lv_obj_update_layout(screen);
    clearFlatPanelsIn(screen);
}

bool drift(unsigned long now, bool allowed) {
    if (pixels == nullptr || tile == nullptr || tilePattern != patternOf(renderedStyle) ||
        (!driftsLeft(renderedStyle) && !driftsDiagonally(renderedStyle))) {
        return false;
    }
    if (!allowed) {
        lastDriftStep = now;
        return false;
    }
    if (now - lastDriftStep < DRIFT_STEP_MS) {
        return false;
    }
    lastDriftStep = now;
    driftX = (driftX + 1) % tileSize; // the picture moves left
    if (driftsDiagonally(renderedStyle)) {
        driftY = (driftY + tileSize - 1) % tileSize; // ...and down
    }
    drawTile();
    return true;
}

} // namespace backdrop
