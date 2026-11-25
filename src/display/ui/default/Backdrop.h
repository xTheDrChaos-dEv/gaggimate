#ifndef BACKDROP_H
#define BACKDROP_H

#include <lvgl.h>

// What is drawn behind the round display's screens. The colours always come from the selected theme; the style is
// its own setting (Settings::backgroundStyle), so any theme can be combined with any background:
//   - a plain colour,
//   - coffee-bean outlines or line-art flowers on the theme colour, still or drifting slowly across the dial,
//   - a generated picture per theme (crema swirls, contour lines, a night sky, paper, thin rings).
// Everything except the plain colour is drawn into one 480x480 RGB565 buffer in PSRAM that every screen uses as
// its background image, so normal redraws stay a plain copy.
namespace backdrop {

// Stored in settings and sent to the web UI: do not renumber.
enum Style {
    STYLE_SOLID = 0,          // the flat theme colour
    STYLE_BEANS = 1,          // bean outlines, not moving
    STYLE_BEANS_LEFT = 2,     // bean outlines drifting from right to left
    STYLE_BEANS_DIAGONAL = 3, // bean outlines drifting down to the left
    STYLE_ARTWORK = 4,        // the theme's generated picture
    STYLE_FLORAL = 5,         // line-art flowers and leaves, not moving
    STYLE_FLORAL_LEFT = 6,    // ...drifting from right to left
    STYLE_FLORAL_DIAGONAL = 7, // ...drifting down to the left
    STYLE_COUNT
};

// Draws the background for a theme (index into theme_colors) and style. Cheap to call again with the same pair.
void render(int theme, int style);

// Makes a screen-sized object use the current background: the shared picture, or nothing for the plain colour (the
// object then shows its own theme colour again). Safe to call on every screen load.
void applyTo(lv_obj_t *screen);

// Several screens lay their content on a large disc filled with the flat theme colour, which would hide the
// picture. This makes such panels (and only those: big, opaque, in a theme background colour) see-through.
void clearFlatPanels(lv_obj_t *screen);

// Moves a drifting background on by one step when it is due. Returns true when the picture changed and the visible
// screen needs redrawing. Pass allowed = false to hold it still (while a shot runs, in standby).
bool drift(unsigned long now, bool allowed);

// Tells the backdrop how long the step it just reported took, from moving the picture to the screen being redrawn.
// Every pixel changes on a step, so it is the most expensive redraw there is; the steps are spaced out so that
// drifting never takes more than about a third of the display task's time, whatever the panel's speed.
void noteDriftCost(unsigned long ms);

// The current pause between two steps (ms), for logs and tests.
unsigned long driftInterval();

} // namespace backdrop

#endif // BACKDROP_H
