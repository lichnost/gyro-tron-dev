"""Reference material the Gyro-Tron dev MCP server serves and builds from.

Kept out of server.py so the durable knowledge (the game template, the constraint
digest) can be read and reviewed on its own. Everything here is English, house style.
"""

# The minimal game the create_game tool starts every project from. It is a real, working
# tilt game - ball physics, tap scoring, the shared pause menu, a saved best, a banner,
# dirty-flag redraws - so an agent starts by editing something that already runs rather
# than by writing from an empty page. Placeholders (__ID__ and friends) are filled in by
# the server; the accent in the descriptor must match game.json's.
GAME_TEMPLATE_C = '''\
// __NAME__ - <one line: what the player does and what the goal is>.
//
// <A short paragraph on the rules, and anything about the design worth remembering.
//  Say why, not what - the code below already says what.>
//
//   tilt          - steer the ball
//   tap           - score a mark
//   swipe left    - pause menu
//
// Written against tat_api.h alone. Built as a package with tools/mktat.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */
#define CC (CW / 2)

// The ball roams inside this radius. The rim sits a little inside the canvas so the ball
// never reaches the corners the round screen cuts off.
#define R_FIELD (CC - 6)
#define R_BALL 7
#define PUSH 260.0f        // how hard a tilt pushes, px/s per g
#define BOUNCE 0.6f        // what the rim takes off a bounce

static struct {
    tat_canvas_t *cv;
    uint8_t c_bg, c_field, c_rim, c_text, c_dim, c_panel, c_accent, c_ball;

    float x, y, vx, vy;    // canvas pixels, and pixels per second
    int marks;
    int best;
    bool dirty;
} g;

// ---------------------------------------------------------------- sound

static void sfx_mark(void)
{
    const tat_tone_t t = {.f0 = 660, .f1 = 880, .ms = 90, .wave = TAT_TRIANGLE, .volume = 0.7f};
    T->tone(&t);
}

// ---------------------------------------------------------------- the game

static void reset_ball(void)
{
    g.x = CC;
    g.y = CC - 40;
    g.vx = 0;
    g.vy = 0;
}

// Commit the round: the best only moves when the round ends, not while it is going.
static void finish_round(void)
{
    if (g.marks > g.best) {
        g.best = g.marks;
        T->save_set("best", g.best);
    }
    g.marks = 0;
    reset_ball();
    g.dirty = true;
}

static void tp_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the canvas");
        return;
    }
    g.c_bg = T->canvas_color(g.cv, T->rgb(8, 9, 16));
    g.c_field = T->canvas_color(g.cv, T->rgb(16, 18, 30));
    g.c_rim = T->canvas_color(g.cv, T->rgb(52, 58, 84));
    g.c_text = T->canvas_color(g.cv, T->rgb(246, 248, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(140, 148, 180));
    g.c_panel = T->canvas_color(g.cv, T->rgb(10, 12, 22));
    g.c_accent = T->canvas_color(g.cv, T->rgb(__ACCENT_R__, __ACCENT_G__, __ACCENT_B__));
    g.c_ball = T->canvas_color(g.cv, T->rgb(255, 214, 61));

    T->save_get("best", &g.best, 0);
    reset_ball();
    T->log("ready, best %d", g.best);
}

static void tp_enter(void) { g.dirty = true; }

static void tp_update(float dt)
{
    if (!g.cv) return;
    if (dt > 1.0f / 20) dt = 1.0f / 20;

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: T->menu_toggle_sound(); break;
        case 1:
            finish_round();
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.dirty = true;
        return;
    }

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();

    if (ges->swipe_left) {
        T->menu_open();
        return;
    }
    if (ges->tap) {
        g.marks++;
        sfx_mark();
        g.dirty = true;
    }

    g.vx += in->tilt.ax * PUSH * dt;
    g.vy += in->tilt.ay * PUSH * dt;
    g.x += g.vx * dt;
    g.y += g.vy * dt;

    // Bounce off the rim: reflect across the rim's normal, keeping BOUNCE of the hit.
    const float dx = g.x - CC, dy = g.y - CC;
    const float d = sqrtf(dx * dx + dy * dy);
    const float max = R_FIELD - R_BALL;
    if (d > max && d > 0) {
        const float nx = dx / d, ny = dy / d;
        g.x = CC + nx * max;
        g.y = CC + ny * max;
        const float vn = g.vx * nx + g.vy * ny;
        if (vn > 0) {
            g.vx -= vn * nx * (1 + BOUNCE);
            g.vy -= vn * ny * (1 + BOUNCE);
            g.dirty = true;
        }
    }
}

static void tp_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        char best[16];
        snprintf(best, sizeof(best), "%d", g.best);
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"RESTART GAME", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", best, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 3, "PAUSED");
        return;
    }

    if (!g.dirty) return;
    g.dirty = false;

    // The field: a rim ring, then the darker disc inside it. A static screen only
    // redraws when a dirty flag says something moved.
    T->canvas_fill_circle(g.cv, CC, CC, R_FIELD, g.c_rim);
    T->canvas_fill_circle(g.cv, CC, CC, R_FIELD - 2, g.c_field);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", g.marks);
    T->canvas_text_centered(g.cv, CC, 24, buf, g.marks > 0 ? g.c_text : g.c_dim, 2, true);

    T->canvas_fill_circle(g.cv, (int)g.x, (int)g.y, R_BALL, g.c_ball);

    if (g.marks == 0) {
        const tat_banner_t b = {
            .top = "TILT ME",
            .mid = "tap anywhere to score",
            .top_color = g.c_accent, .mid_color = g.c_dim,
            .panel = g.c_panel,
            .bars = true,
        };
        T->canvas_banner(g.cv, CC, CC - 20, CW - 60, &b);
    }

    T->canvas_present(g.cv);
}

static void tp_redraw(void) { g.dirty = true; }

// The screen is static between taps, so the game never asks to keep the watch awake.
static bool tp_keep_awake(void) { return false; }

static void tp_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "__ID__",
    .name = "__NAME__",
    .accent_r = __ACCENT_R__, .accent_g = __ACCENT_G__, .accent_b = __ACCENT_B__,
    .assets = NULL,
    .asset_count = 0,
    .begin = tp_begin,
    .enter = tp_enter,
    .update = tp_update,
    .draw = tp_draw,
    .leave = NULL,
    .unload = tp_unload,
    .redraw = tp_redraw,
    .keep_awake = tp_keep_awake,
    .state_save = NULL,
    .state_load = NULL,
};
'''

GAME_JSON_TEMPLATE = '''\
{
  "id": "__ID__",
  "name": "__NAME__",
  "author": "__AUTHOR__",
  "version": 1,
  "status": "dev",
  "api_major": __API_MAJOR__,
  "api_minor": __API_MINOR__,
  "accent": [__ACCENT_R__, __ACCENT_G__, __ACCENT_B__]
}
'''

# The hard constraints, digested for an agent. Served as tat://constraints. This is the
# short list a game has to satisfy; docs/reference/GAME_API.md remains the full contract. The
# __EXPORT_LIST__ placeholder is filled from the loader's export table at read time -
# the one list here that grows as games need more.
CONSTRAINTS_MD = """# Gyro-Tron game constraints (digest)

## The device
- Waveshare ESP32-S3-Touch-AMOLED-1.75: ESP32-S3, two cores at 240 MHz, 8 MB PSRAM,
  32 MB flash. A battery watch: it sleeps when idle, PWR boots it from cold.
- Round 466x466 AMOLED; single-point capacitive touch; a 6-axis IMU (accelerometer in g,
  gyroscope in deg/s, both screen-aligned); a small speaker behind a chiptune-style
  synth. No vibration, no Bluetooth, no GPS - plan around tilt, turn, touch, the two
  keys and sound.
- Frame loop is vblank-locked at ~60 Hz: `update(dt)` then `draw()`, dt in seconds
  (~1/60). Clamp dt - a hitch hands you one big frame; the template clamps to 1/20.

## Screen and canvas
- Round screen, 466x466 device pixels. At the bottom (y=441) the visible chord is only
  +/-105 px wide - keep interactive content well inside the circle.
- Games draw on a palette canvas: 8-bit indices, `canvas_create(2, 0)` gives 233x233
  (every canvas pixel = 2x2 screen pixels), scale 3 gives 155x155. Present with
  `canvas_present` or `canvas_present_rotated`.
- Palette index 0 is transparent in sprites and draws black. Get colours from
  `T->rgb(r,g,b)` + `canvas_color()` - never pack pixels by hand.
- Text: bold at scale >= 2. `text_width()` measures without drawing.

## The contract
- A game includes `"tat/tat_api.h"` and nothing else from the console (plain C system
  headers are fine). No C++: fixed-layout POD structs, no inline functions across the ABI,
  file-scope `const` must be a constant expression in C.
- Exactly one `extern const tat_game_t tat_game`. `begin(const tat_api_t *api)` stores the
  api pointer; it stays valid until `unload`. Frame order: `update(dt)` then `draw()`.
- API versioning: set `api_major = TAT_API_MAJOR`, `api_minor = TAT_API_MINOR` from the
  header. An OS runs the game iff major matches and game minor <= OS minor. New API
  functions are only ever appended to the end of `tat_api_t`.
- What a game may call by name is frozen: the api table plus one C library list, taken
  from the loader's export table -
  __EXPORT_LIST__.
  Anything outside it - qsort, sscanf, the ctype functions, printf, double-only maths
  (pow, sin), rand - compiles in the PC emulator and packs without a warning, and the
  watch then refuses to load the game. Use `T->random()`, `snprintf`, the float maths.

## Memory and CPU
- One game in memory; nothing allocates per frame. Big state (KB and up) goes through
  `T->alloc()` in `begin` - statics land in scarce internal RAM that Wi-Fi needs.
  Everything from `alloc` is freed automatically at `unload`.
- Numbers to plan against: `T->alloc()` draws from the 8 MB PSRAM, shared with the
  framebuffer, the loaded game's code and every canvas and sheet - a few MB is fine,
  8 is not there. A 233x233 canvas costs ~54 KB, the polar tables ~870 KB.
- Fixed arrays with swap-with-last removal; after `a[i] = a[--n]` do NOT advance i.
  Decide what happens when an array fills before it does.
- Screens are static between state changes: redraw through dirty flags, no animation
  loops. `keep_awake()` returns true only while the player is mid-action.

## Input and shared UX
- Tap is primary; PWR (TAT_BTN_B) is the game's secondary action; BOOT never reaches the
  game (it is home). Swipe left = open the console pause menu; swipe right = back.
- Touch is one point (down/pressed/released + x, y) - no multi-touch, no pressure.
- The pause menu and the banner are the console's (`menu_*`, `canvas_banner`) - use them,
  do not roll your own. Bold text at scale >= 2.
- Input structs are valid for this frame only.

## Packaging
- `games/<id>/`: `game.json` (id, name, author, version, api_major, api_minor, accent),
  one `<id>.c` (more .c files allowed, none named `*_builtin.c`), `icon.png`, optional
  `assets/` of PNG sprite sheets.
- `id`: lowercase [a-z0-9_], max 15 chars - it is also the save namespace. Check the
  existing games before choosing one. `name`/`author`: max 23 chars each.
- icon.png: 210x210 drawn in a circle (launcher decodes at most 232x232), at most 4096
  bytes. Asset names max 15 bytes.
- The package lands as one file in the watch's /Games on a 16 MB filesystem it shares
  with themes and faces - a few MB is a big game here. At launch the CODE section is
  copied into PSRAM and mapped executable (64 KB aligned), so code size costs PSRAM;
  ASSETs are used in place and DATA is streamed, so a package may carry more than RAM
  holds.
- Build with `python tools/mktat.py games/<id>` -> `build/<id>.tat`. Test on PC with
  `python tools/emu/emu.py play <id>` / `shot <id> "<script>"`.

## Data
- Saves: `save_get/save_set` integer key/value in the game's own namespace (its id);
  erased with the game. No filesystem access otherwise.
- Sleep snapshots (API 1.4+): `state_save/state_load`, up to 32 KB, version the blob,
  no pointers, return false rather than guess.
- Assets come from `T->asset(name, &len)` in place; one big `DATA` blob is streamed with
  `data_read/data_size`.

## Sound
- `T->tone(&t)` with a `tat_tone_t` (f0, f1, ms, wave, volume, delay_ms) - little
  melodies by chaining delays. The synth never blocks the game loop.
- Three waves: square (punchy blip), triangle (softer), noise (hits, explosions).
- The player owns the volume: 0 OFF, 1 LOW, 2 MED, 3 HIGH, persisted - `volume()` reads
  it, your tone's `volume` is relative to it.

## Power
- The watch sleeps after ~120 s idle (a console setting) unless `keep_awake()` is true;
  `leave()` runs at sleep, `enter()` at wake - both must leave the game playable.
  Power-off is deep sleep: memory dies, and the `state_save` snapshot is what comes back.
- A game never sees the battery, the charger or the clock - `now_us()` is monotonic.
  Charging is USB only.
"""

# What create_game's icon looks like: an accent disc with a dark rim and a light dot
# marking "up", echoing the manager app's own icon. Well under the 4096-byte limit.
