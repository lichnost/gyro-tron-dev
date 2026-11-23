// The contract between a Gyro-Tron and a game.
//
// A game includes this header and nothing else from the console. It never calls the OS by
// name: everything arrives through the one `tat_api_t *` handed to begin(). That is what
// lets a game built today keep running on next year's firmware - the only addresses that
// need fixing when it loads are its own.
//
// Plain C, fixed-layout structs, no inline functions. Anything added here is added at the
// END of tat_api_t and nowhere else: an old game reading a grown struct only calls what it
// already knew about. (1.7 broke this once, removing the never-called go_home before the
// console had shipped anything - every package had to be rebuilt against the new layout.)
//
// See docs/reference/GAME_API.md for how a game becomes an installable file.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAT_API_MAJOR 1
#define TAT_API_MINOR 9

// ---------------------------------------------------------------- basics

#define TAT_SCREEN 466   // the round display, in device pixels

// A colour in whatever the panel wants. Games never build one by hand - they ask the
// console with api->rgb(), so the packing stays the console's business.
typedef uint16_t tat_color_t;

typedef struct tat_canvas tat_canvas_t;   // opaque
typedef struct tat_sheet tat_sheet_t;     // opaque

enum {
    TAT_BTN_A = 1 << 0,   // BOOT, the small key by the USB port. The console takes this
                          // one for "home", so a game never sees it held.
    TAT_BTN_B = 1 << 1,   // PWR, the game's secondary action
};

typedef struct {
    float ax, ay, az;   // gravity in g, screen-aligned: +x right, +y down
    float gx, gy, gz;   // turn rate, degrees per second - one instantaneous sample
    float rx, ry, rz;   // turn over the frame, degrees: the gyro integrated at the IMU's
                        // own rate since the last frame, corrected like gx. An orientation
                        // filter must turn by THIS, not by gx*dt - the wrist speeds up and
                        // slows down inside a frame, one sample stretched over dt misses
                        // most of a sweep, and heading has no reference to correct it back
    float rdt;          // seconds rx accumulated over (one frame on hardware)
} tat_tilt_t;

typedef struct {
    uint8_t down, pressed, released;
    int32_t x, y;
} tat_touch_t;

typedef struct {
    uint32_t held, pressed, released, clicked, long_press, double_clicked;
    tat_touch_t touch;
    tat_tilt_t tilt;
} tat_input_t;

// Set for exactly one frame when a finger lifts. Swipe left opens the pause menu by
// convention; swipe right means back, everywhere on the console.
typedef struct {
    uint8_t tap, swipe_left, swipe_right;
    int32_t x, y;   // where the gesture started
} tat_gestures_t;

typedef enum { TAT_SQUARE = 0, TAT_TRIANGLE = 1, TAT_NOISE = 2 } tat_wave_t;

typedef struct {
    float f0, f1;        // start and end frequency in Hz; f1 = 0 holds f0
    uint16_t ms;
    uint8_t wave;        // tat_wave_t
    float volume;        // 0..1, relative to the console's volume setting
    uint16_t delay_ms;   // wait before it sounds, for little melodies
} tat_tone_t;

// ---------------------------------------------------------------- the pause menu
//
// Every game's pause screen is the console's, not its own: same title, same rows, same
// RESUME and HOME. A game says what its rows are and is told which one was tapped.

typedef struct {
    const char *label;
    const char *value;
    tat_color_t color;   // 0 uses the console's usual value colour
} tat_menu_row_t;

enum {
    TAT_MENU_NONE = -1,     // nothing happened this frame
    TAT_MENU_CLOSED = -2,   // the player resumed or went home; the menu is already shut
};

// Named colours, so a game's menu matches the console and follows the player's theme.
typedef enum {
    TAT_UI_TEXT = 0,
    TAT_UI_LABEL,
    TAT_UI_DIM,
    TAT_UI_VALUE,
    TAT_UI_ACCENT,
    TAT_UI_GO,
    TAT_UI_DANGER,
} tat_ui_color_t;

// ---------------------------------------------------------------- the banner
//
// The little panel a game drops over the play area to say something: LEVEL 3, GAME OVER,
// a final score. Shared for the same reason the pause menu is - a console where every
// game announces itself differently doesn't feel like one console. Three lines at most,
// on a panel only as tall as the lines it holds.

typedef struct {
    const char *top;             // always drawn, bold
    const char *mid, *bottom;    // NULL to leave out
    uint8_t top_color, mid_color, bottom_color;   // palette indices
    uint8_t panel;               // the fill
    uint8_t border;              // the outline, or the bars when `bars` is set
    uint8_t top_scale;           // 0 or 1 for a line, 2 for a headline
    bool bars;                   // accent bars above and below instead of an outline
    bool bottom_bold;
} tat_banner_t;

// ---------------------------------------------------------------- what a game gets

typedef struct tat_api {
    uint16_t major, minor;

    // ---- console
    tat_color_t (*rgb)(uint8_t r, uint8_t g, uint8_t b);
    void (*log)(const char *fmt, ...);
    int64_t (*now_us)(void);
    uint32_t (*random)(void);

    // ---- input, valid for this frame only
    const tat_input_t *(*input)(void);
    const tat_gestures_t *(*gestures)(void);

    // ---- memory. Everything taken here is given back when the game unloads, so a game
    // that forgets cannot leak across launches.
    void *(*alloc)(size_t bytes);
    void (*free)(void *p);

    // ---- the canvas: a small palette picture the console scales onto the round screen.
    // `scale` is 2 (233x233) or 3 (155x155). `size` 0 fits the screen exactly; a larger
    // one is centred, which gives canvas_present_rotated something to show in the corners.
    tat_canvas_t *(*canvas_create)(int scale, int size);
    void (*canvas_destroy)(tat_canvas_t *c);
    int (*canvas_width)(tat_canvas_t *c);
    uint8_t *(*canvas_pixels)(tat_canvas_t *c);

    // Palette. color() hands back the index for a colour, making a new entry the first
    // time. Index 0 is transparent in sprites and draws black.
    uint8_t (*canvas_color)(tat_canvas_t *c, tat_color_t rgb);
    uint8_t (*canvas_reserve)(tat_canvas_t *c, int n);   // entries the game will set itself
    void (*canvas_set_color)(tat_canvas_t *c, uint8_t i, tat_color_t rgb);

    void (*canvas_clear)(tat_canvas_t *c, uint8_t i);
    void (*canvas_pixel)(tat_canvas_t *c, int x, int y, uint8_t i);
    void (*canvas_fill_rect)(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i);
    void (*canvas_rect)(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i);
    void (*canvas_line)(tat_canvas_t *c, int x0, int y0, int x1, int y1, uint8_t i);
    void (*canvas_fill_circle)(tat_canvas_t *c, int cx, int cy, int r, uint8_t i);
    int (*canvas_text)(tat_canvas_t *c, int x, int y, const char *s, uint8_t i, int scale, bool bold);
    void (*canvas_text_centered)(tat_canvas_t *c, int cx, int cy, const char *s, uint8_t i, int scale, bool bold);

    // Sprite sheets are PNGs of frames side by side; alpha below half is transparent.
    tat_sheet_t *(*sheet_load)(tat_canvas_t *c, const void *png, size_t len, int fw, int fh);
    void (*canvas_sprite)(tat_canvas_t *c, tat_sheet_t *s, int frame, int x, int y, bool flip_x);
    void (*canvas_sprite_scaled)(tat_canvas_t *c, tat_sheet_t *s, int frame, float x, float y,
                                 float sx, float sy, bool flip_x);

    // Put the canvas on the screen. Rotated turns it about the centre, which is how a
    // game keeps the world upright while the watch is turned.
    void (*canvas_present)(tat_canvas_t *c);
    void (*canvas_present_rotated)(tat_canvas_t *c, float radians);

    // ---- the pause menu (see above)
    void (*menu_open)(void);
    void (*menu_close)(void);
    bool (*menu_is_open)(void);
    void (*menu_invalidate)(void);
    int (*menu_update)(void);   // a row index, or TAT_MENU_NONE / TAT_MENU_CLOSED
    void (*menu_draw)(const tat_menu_row_t *rows, int count, const char *title);
    tat_menu_row_t (*menu_sound_row)(void);
    void (*menu_toggle_sound)(void);
    tat_color_t (*ui_color)(int which);   // tat_ui_color_t

    // ---- sound
    void (*tone)(const tat_tone_t *t);
    int (*volume)(void);   // 0 off .. 3 high, the console's setting

    // ---- saved settings and scores, in this game's own namespace. Erased with the game.
    // get() leaves the value alone if nothing is stored, so the caller's initialiser is
    // the default. `limit` > 0 also rejects anything outside 0..limit-1.
    void (*save_get)(const char *key, int *value, int limit);
    void (*save_set)(const char *key, int value);

    // ---- the game's own files, by the name they were packaged under
    const void *(*asset)(const char *name, size_t *len);

    // ---- added in 1.1
    // `cx` is the centre, `y` the top edge, `w` the width, all in canvas pixels.
    void (*canvas_banner)(tat_canvas_t *c, int cx, int y, int w, const tat_banner_t *b);

    // ---- added in 1.2: where every screen pixel is, in polar coordinates
    //
    // The screen is round, so a lot of what looks good on it is rings, arcs and wedges -
    // and working out an angle and a distance per pixel with atan2 and sqrt is far too slow
    // to do every frame. These are two tables of TAT_SCREEN * TAT_SCREEN entries, row major,
    // indexed y * TAT_SCREEN + x. They are handed over whole rather than a call per pixel,
    // because a game shading the screen would otherwise make millions of indirect calls a
    // second to read two array elements.
    //
    // angle 0..65535 spans a full turn, measured like atan2(dy, dx) with +y down the screen.
    // radius is in 1/16 px from the true centre. Both return NULL if the tables could not be
    // built (they cost about 870 KB), and a game that uses them has to cope with that.
    //
    // A canvas pixel (x, y) at scale s is screen pixel (s*x, s*y): index (s*y)*TAT_SCREEN + s*x.
    const uint16_t *(*polar_angles)(void);
    const uint16_t *(*polar_radii)(void);

    // How wide a string would be, without drawing it. canvas_text returns where it ended,
    // which is no help at all when the thing you are deciding is where to start it.
    int (*text_width)(const char *s, int scale, bool bold);

    // A sheet's own pixels, for a game that has to blit one itself rather than just place
    // a frame - recolouring it, or shearing it row by row. Palette indices, row major, `w`
    // wide, 0 transparent. This is how one sprite becomes many liveries without
    // shipping repeated PNGs of the same art.
    const uint8_t *(*sheet_pixels)(tat_sheet_t *s);
    void (*sheet_info)(tat_sheet_t *s, int *w, int *h, int *fw, int *fh);

    // ---- added in 1.3
    // Install a whole palette at once, so the game owns its indices outright. canvas_color
    // hands out whatever is next and canvas_reserve a block starting wherever it happens to
    // start, which is fine for a game that just wants colours - but a game whose sprite
    // sheets are drawn against known index numbers, and which compares pixels against them
    // to recolour a sprite, needs the indices to be the ones it chose. Index 0 stays black
    // and transparent whatever is passed for it. Call it before loading any sheet, so the
    // sheet's colours land on these entries instead of new ones.
    void (*canvas_set_palette)(tat_canvas_t *c, const tat_color_t *pal, int n);

    // ---- added in 1.5
    // The console's SENSITIVITY setting (Settings), as a factor the same physical turn
    // should feel stronger by: 1.0 when the setting is at X1. The console does not scale
    // the tilt it delivers - a game's input may be an angle, a threshold or raw gravity,
    // and each of those wants the gain in a different place. Divide by this whatever you
    // treat as "full tilt": a threshold becomes reached sooner, a gain becomes larger,
    // and a direction-only game can ignore it entirely.
    float (*tilt_sensitivity)(void);

    // ---- added in 1.6: streamed PCM, for a game that renders its own sound
    //
    // tone() is for one-shot effects; a game with a synth of its own - an emulator's
    // APU, a tracker - renders mono int16 samples and pushes them here instead, and
    // the mixer blends the stream with tones under the console's volume setting.
    //
    // start() opens the stream and returns the rate the console plays at. Pass the
    // rate you will generate at, or 0 to take the console's own, which is the one
    // rate nothing is resampled at. 0 comes back when there is no audio path, and a
    // game that gets it stays silent. One stream at a time: start() again - a pause
    // menu closing, say - drops what was still queued and keeps going.
    //
    // push() queues `count` samples and returns how many were taken. The buffer is
    // small - about a tenth of a second - so a game running ahead gets 0 back and
    // drops its render rather than blocking; space() says how much fits right now,
    // for skipping the work of rendering sound nobody will hear. stop() shuts the
    // stream and drops what is buffered, which is how a pause menu goes silent at
    // once instead of playing out whatever was still queued.
    int (*audio_start)(int rate);
    int (*audio_push)(const int16_t *samples, int count);
    int (*audio_space)(void);
    void (*audio_stop)(void);

    // ---- added in 1.9: chunked asset reads, for a game that carries one big bundle
    // rather than many small files. asset() serves its pointer by materializing the
    // whole file in PSRAM, which a multi-MB bundle cannot afford - one can take half
    // the watch's free PSRAM on its own. These read a range straight from the
    // package instead: nothing is held beyond the bytes the game itself keeps.
    // size() is 0 when the name is unknown; read() returns the bytes read (0 at
    // end of file) or -1, and reads past the end are clamped.
    int (*asset_size)(const char *name);
    int (*asset_read)(const char *name, int offset, void *dst, int n);
} tat_api_t;

// ---------------------------------------------------------------- what a game exports

#define TAT_GAME_MAGIC 0x47544154u   // 'TATG'

// A file the game carries: a sprite sheet, a level, anything. Built into the firmware for
// a built-in game; unpacked from the .tat for an installed one. Same to the game either way.
typedef struct {
    const char *name;
    const uint8_t *data;
    const uint8_t *end;   // one past the last byte. A length would read better, but the
                          // difference of two linker symbols is not a constant expression
                          // in C, so a built-in game could not write its table down.
} tat_asset_t;

typedef struct {
    uint32_t magic;
    uint16_t api_major, api_minor;

    const char *id;      // "pindrop" - lowercase; also the save namespace
    const char *name;    // "PIN DROP" - what the carousel shows
    // The game's colour, for the launcher. Plain bytes, not a tat_color_t: a descriptor
    // is built before the game has an api pointer to ask rgb() with, and a game writing
    // the panel's packing by hand is exactly what rgb() exists to prevent.
    uint8_t accent_r, accent_g, accent_b;

    const tat_asset_t *assets;
    int asset_count;

    void (*begin)(const tat_api_t *api);   // once, after loading. Keep the pointer.
    void (*enter)(void);                   // becoming the active app; the screen is black
    void (*update)(float dt);              // once per frame
    void (*draw)(void);                    // once per frame, after update
    void (*leave)(void);                   // another app took over, or the watch is sleeping
    void (*unload)(void);                  // being removed from memory; let go of things
    // Optional. A game that only draws when something changes gets this when the console
    // needs a frame anyway - a screenshot over Wi-Fi, or the screen coming back on.
    void (*redraw)(void);
    bool (*keep_awake)(void);              // optional: true while the player is mid-action

    // ---- added in 1.4, optional: sleep snapshots.
    //
    // Deep sleep keeps no memory, so before it the console asks the game to write its
    // logical state - a level, a score, a board - into `buf` and return how many bytes it
    // used. 0 (or leaving these NULL) means "nothing worth keeping", and the game starts
    // fresh when the watch next comes on. state_load gets those bytes back after begin(),
    // and returns false if it cannot make sense of them, in which case the game also
    // starts fresh. Don't write pointers or anything derived; rebuild those in begin().
    // Version the blob inside the game, and ignore (return false) rather than guess.
    //
    // Only called by a console that offers API 1.4 or later, so a game that uses them
    // should require api_minor >= 4 - and an older console never reads these fields.
    int (*state_save)(void *buf, size_t max);
    bool (*state_load)(const void *buf, size_t len);
} tat_game_t;

// Every game defines exactly one of these, named tat_game.
extern const tat_game_t tat_game;

// A game with files of its own also declares this and points its descriptor at it. Where
// the table comes from is the build's business, not the game's: linked into the firmware
// for a built-in game, supplied by the package for an installed one. Either way the game
// asks api->asset() for its files by the name it packaged them under.
extern const tat_asset_t tat_assets[];

#ifdef __cplusplus
}
#endif
