// The Gyro-Tron emulator's core: a game, and the console's own code around it, built as
// one library for a PC.
//
// It is not a lookalike. The canvas, the font, the banners, the pause menu, the save store
// and the whole game API (components/tat_api/tat_host.cpp) are the firmware's sources,
// compiled unchanged. What is replaced is the hardware under them, and there is not much:
// the presenter that would stream pixels to the panel collects them into a picture here,
// the engine's input comes from whoever is driving this, sound is a line in the log, and
// NVS is a table written to a file. So what a game draws here is what it draws on a watch,
// to the pixel, and a game that misbehaves here misbehaves there.
//
// What this cannot tell you is how a game feels in the hand, how fast the real chip runs
// it, or anything about the loader - a game is linked in here, not loaded from a .tat.
//
// tools/emu/emu.py builds this and drives it; see tools/emu/README.md.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "engine/autocal.h"
#include "engine/calib_fit.h"
#include "engine/engine.h"
#include "engine/gfx.h"
#include "engine/presenter.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "loader/loader.h"
#include "nvs.h"
#include "tat/tat_api.h"
#include "tat/tat_host.h"

#ifdef _WIN32
#define EMU_API extern "C" __declspec(dllexport)
#else
#define EMU_API extern "C" __attribute__((visibility("default")))
#endif

// Built around a game - or, with EMU_SCREEN_*, around one of the console's own screens, which
// are not games and draw through the engine rather than the game API. A screen is compiled
// alongside only the console sources it stands on (emu.py's SCREEN_SOURCES), so each pulls
// in just its own headers.
#if defined(EMU_SCREEN_BOOT) || defined(EMU_SCREEN_POWER) || defined(EMU_SCREEN_HOLD) || defined(EMU_SCREEN_HOME) || \
    defined(EMU_SCREEN_UPDATE) || defined(EMU_SCREEN_CAL) || defined(EMU_SCREEN_STORE)
#define EMU_SCREEN 1
#endif
#ifdef EMU_SCREEN_BOOT
#include "console/boot_anim.h"
#endif
#if defined(EMU_SCREEN_POWER) || defined(EMU_SCREEN_HOLD)
#include "console/power_menu.h"
#endif
#ifdef EMU_SCREEN_HOME
#include "console/icons.h"
#include "console/launcher.h"
#include "console/theme.h"
#include "console/ui.h"
#endif
#ifdef EMU_SCREEN_UPDATE
#include "console/update_app.h"
#endif
#ifdef EMU_SCREEN_CAL
#include "console/calibrate_app.h"
#include "console/console.h"
#endif
#ifdef EMU_SCREEN_STORE
#include "console/store_app.h"
#endif
#ifdef EMU_SCREEN_STORE
#include "console/store_app.h"
#endif
#ifndef EMU_SCREEN
extern "C" const tat_game_t tat_game;   // the game this library was built around
#endif

// A game names its files through `tat_assets`, which on a watch the loader fills in from the
// package. Here it is filled in from the game's assets folder. The game sees it as const;
// it is defined in emu_assets.c, which does not, so that it can be written.
extern "C" void emu_set_asset(int slot, const char *name, const unsigned char *data, const unsigned char *end);

// ------------------------------------------------------------------ time, chance, the log

static int64_t s_now_us = 1000000;   // not zero: games seed things from it
static uint32_t s_rng = 0x2F6E2B1u;
static bool s_quiet = false;
static std::string s_log_pending;   // drained by the Python driver (emu_log_take / wait_log)

int64_t esp_timer_get_time(void) { return s_now_us; }

uint32_t esp_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

void emu_log_line(char level, const char *tag, const char *fmt, ...)
{
    static bool s_tail_atexit = false;
    if (!s_tail_atexit) {
        s_tail_atexit = true;
        // The game calling exit() - or faulting out of it - ends the process without the
        // driver ever draining the pile, and that undrained tail is the crash's own
        // trail. Print it at exit even under --quiet, or a hard failure dies silent.
        std::atexit([] {
            if (!s_log_pending.empty()) {
                std::fwrite(s_log_pending.data(), 1, s_log_pending.size(), stdout);
                std::fflush(stdout);
                s_log_pending.clear();
            }
        });
    }
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    // The pending pile is kept even when quiet: --quiet silences the console, not the
    // data - a scripted run matching log lines (wait_log) must see them regardless.
    s_log_pending += level;
    s_log_pending += " (" + std::to_string(s_now_us / 1000) + ") " + tag + ": " + line + "\n";
    if (s_quiet) return;
    std::printf("%c (%lld) %s: %s\n", level, (long long)(s_now_us / 1000), tag, line);
    std::fflush(stdout);
}

// Hands over at most `cap` bytes of log the game produced since the last take and
// consumes them; returns how many. The same shape as emu_audio_take - the driver
// drains, the library never blocks.
EMU_API int emu_log_take(char *dst, int cap)
{
    const int n = std::min(cap, int(s_log_pending.size()));
    std::copy(s_log_pending.begin(), s_log_pending.begin() + n, dst);
    s_log_pending.erase(s_log_pending.begin(), s_log_pending.begin() + n);
    return n;
}

// ------------------------------------------------------------------ sound
//
// Tones are a line in the log. A PCM stream piles up in a vector for the driver to take
// (emu_audio_take) and write to a WAV - and to play live when it can. Nothing here drains
// in real time, so the emulator keeps every sample the game generated instead of mirroring
// the hardware ring's backpressure: a scripted run runs faster than the mixer would drain,
// and keeping only what fitted would leave a WAV holding the last tenth of a second.

namespace wc::audio {
static int s_volume = 2;
static int s_tones = 0;
static std::vector<int16_t> s_stream;
static int s_stream_rate = 0;
bool init() { return true; }
void play(const Tone &t)
{
    s_tones++;
    if (s_volume == 0) return;
    static const char *const W[] = {"square", "triangle", "noise"};
    emu_log_line('I', "audio", "%s %.0f->%.0f Hz, %u ms, vol %.2f, after %u ms", W[int(t.wave) % 3], t.f0, t.f1, t.ms,
                 t.volume, t.delay_ms);
}
void play(const Tone *tones, int count)
{
    for (int i = 0; i < count; i++) play(tones[i]);
}
void stopAll() {}
int volume() { return s_volume; }
void setVolume(int level) { s_volume = level; }
int streamStart(int rate)
{
    s_stream_rate = rate > 0 ? rate : 22050;
    return 22050;
}
int streamWrite(const int16_t *samples, int count)
{
    if (s_stream_rate == 0 || count <= 0) return 0;
    s_stream.insert(s_stream.end(), samples, samples + count);
    return count;
}
int streamSpace() { return s_stream_rate == 0 ? 0 : 1 << 20; }   // a PC buffer is effectively unbounded
void streamStop() { s_stream_rate = 0; }
}  // namespace wc::audio

EMU_API int emu_audio_rate(void) { return 22050; }

// Hands over at most `max` samples and consumes them; returns how many.
EMU_API int emu_audio_take(int16_t *dst, int max)
{
    const int n = std::min(max, int(wc::audio::s_stream.size()));
    std::copy(wc::audio::s_stream.begin(), wc::audio::s_stream.begin() + n, dst);
    wc::audio::s_stream.erase(wc::audio::s_stream.begin(), wc::audio::s_stream.begin() + n);
    return n;
}

// ------------------------------------------------------------------ the home screen's footing

#ifdef EMU_SCREEN_HOME
// The launcher stands on theme.cpp, console.cpp and the board's PMU, none of which belong
// in an emulator build: the theme is files on the watch's drive (and pulls in cJSON and
// dirent), and the PMU is a chip. What the launcher needs of them is small, and stands in
// here: no theme means the built-in look, no app is hidden, the drive never changes, and
// the battery is told through the environment - EMU_BATTERY, EMU_CHARGING, EMU_USB - so a
// scripted run can show the home screen in any state of charge.

// A few apps to hang the carousel on: the firmware's own code-drawn icons, no games behind
// them. The home screen is what's under test, not anything it launches.
static console::App k_apps[] = {
    {"breakout", "BREAKOUT", wc::colors::yellow, console::icons::breakout, nullptr},
    {"maze", "MARBLE MAZE", wc::colors::cyan, console::icons::maze, nullptr},
    {"clock", "CLOCK", wc::colors::white, console::icons::clock, nullptr},
    {"settings", "SETTINGS", wc::colors::gray, console::icons::settings, nullptr},
};

namespace console {

void Image::release()
{
    heap_caps_free(px);
    heap_caps_free(alpha);
    px = nullptr;
    alpha = nullptr;
    w = h = 0;
}

void drawImage(wc::Gfx &g, const Image &img, int x, int y)
{
    if (!img.valid()) return;
    if (img.alpha) g.blitAlpha(img.px, img.alpha, img.w, img.h, x, y);
    else g.blit(img.px, img.w, img.h, x, y);
}

static Theme s_theme;
Theme &Theme::get() { return s_theme; }
void Theme::reloadWithProgress(wc::Engine &) {}
const Image *Theme::icon(const std::string &, const std::string &) const { return nullptr; }
bool Theme::builtinIcon(const std::string &, Image &) { return false; }
bool Theme::iconFromPng(const uint8_t *, size_t, const char *, Image &) { return false; }
bool appHidden(const char *) { return false; }

}  // namespace console

namespace console::ui {
void clearScreen(wc::Gfx &g, int, int) { g.clear(BG); }
void restoreBg(wc::Gfx &g, int x, int y, int w, int h, int, int) { g.fillRect(x, y, w, h, BG); }
}  // namespace console::ui

extern "C" {
uint32_t storage_generation(void) { return 0; }

// The launcher's UPDATE hint asks whether the site is offering something. EMU_OTA turns
// it on the same way it drives the update screen.
bool ota_pull_pending(char *version, size_t len)
{
    const char *mode = std::getenv("EMU_OTA");
    const bool on = mode && mode[0] && std::strcmp(mode, "uptodate") != 0 && std::strcmp(mode, "failed") != 0;
    if (on && version) snprintf(version, len, "0.9.10");
    return on;
}
}
#endif  // EMU_SCREEN_HOME

// EMU_PSRAM_MB: a plain byte budget under heap_caps_*, off by default. No fragmentation
// model - a budget is enough to reach the paths that only run when an alloc is refused.
// Block sizes come from the C runtime, so a pointer needs no header of ours.
#ifdef _WIN32
#include <malloc.h>
#define EMU_BLOCK_SIZE(p) _msize(p)
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define EMU_BLOCK_SIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define EMU_BLOCK_SIZE(p) malloc_usable_size(p)
#endif
static size_t s_psram_budget, s_psram_used, s_psram_peak, s_psram_refused;

static size_t psram_budget()
{
    static const bool once = [] {
        const char *v = std::getenv("EMU_PSRAM_MB");
        s_psram_budget = v ? size_t(std::atof(v) * 1024 * 1024) : 0;
        if (s_psram_budget) {
            std::atexit([] {
                std::printf("psram: budget %zu used %zu peak %zu refused %zu\n", s_psram_budget, s_psram_used,
                            s_psram_peak, s_psram_refused);
                std::fflush(stdout);
            });
        }
        return true;
    }();
    (void)once;
    return s_psram_budget;
}

static void *psram_account(void *p)
{
    if (p) {
        s_psram_used += EMU_BLOCK_SIZE(p);
        if (s_psram_used > s_psram_peak) s_psram_peak = s_psram_used;
    }
    return p;
}

extern "C" {
void *heap_caps_malloc(size_t n, int)
{
    if (psram_budget() && s_psram_used + n > s_psram_budget) {
        s_psram_refused++;
        return nullptr;
    }
    return psram_account(std::malloc(n));
}
void *heap_caps_calloc(size_t n, size_t s, int)
{
    if (psram_budget() && s_psram_used + n * s > s_psram_budget) {
        s_psram_refused++;
        return nullptr;
    }
    return psram_account(std::calloc(n, s));
}
void heap_caps_free(void *p)
{
    if (!p) return;
    const size_t n = EMU_BLOCK_SIZE(p);
    s_psram_used -= n < s_psram_used ? n : s_psram_used;
    std::free(p);
}
size_t heap_caps_get_free_size(int)
{
    return psram_budget() ? s_psram_budget - s_psram_used : 8u << 20;
}
size_t heap_caps_get_largest_free_block(int)
{
    return psram_budget() ? s_psram_budget - s_psram_used : 4u << 20;
}
}

// The emulator links a game straight in, so no package is ever resident: api_asset takes
// its descriptor-table path and these are never reached past loader_resident().
extern "C" {
loader_game_t *loader_resident(void) { return nullptr; }
int loader_asset_count(loader_game_t *) { return 0; }
const char *loader_asset_name(loader_game_t *, int) { return nullptr; }
size_t loader_asset_size(loader_game_t *, int) { return 0; }
int loader_asset_read(loader_game_t *, int, size_t, void *, size_t) { return -1; }
const void *loader_asset_data(loader_game_t *, int, size_t *) { return nullptr; }
}

// The PMU is a chip. The screens that show the battery or ask whether the cable is in
// are told about it through the environment - EMU_BATTERY, EMU_CHARGING, EMU_USB - so a
// scripted run can show any state of charge or of plug.
#if defined(EMU_SCREEN_HOME) || defined(EMU_SCREEN_UPDATE) || defined(EMU_SCREEN_STORE)
extern "C" {
static bool emu_env_flag(const char *name)
{
    const char *v = std::getenv(name);
    return v != nullptr && std::atoi(v) != 0;
}

int pmu_battery_percent(void)
{
    static const int pct = [] {
        const char *v = std::getenv("EMU_BATTERY");
        return v ? std::atoi(v) : 62;
    }();
    return pct;
}
bool pmu_charging(void)
{
    static const bool on = emu_env_flag("EMU_CHARGING");
    return on;
}
bool pmu_usb_power(void)
{
    static const bool on = emu_env_flag("EMU_USB");
    return on;
}
}
#endif

// tat_host hands games api->tilt_sensitivity() from the console's SENSITIVITY setting,
// which lives in console.cpp - not compiled here. EMU_TILT_SENS stands in for it, so a
// scripted run can exercise a game's response at any level (default: the real X1).
namespace console {
float tiltSensGain()
{
    static const float gain = [] {
        const char *v = std::getenv("EMU_TILT_SENS");
        return v && std::atof(v) > 0.0f ? float(std::atof(v)) : 1.0f;
    }();
    return gain;
}
}  // namespace console

// ------------------------------------------------------------------ the update screen

#ifdef EMU_SCREEN_UPDATE
// The update screen stands on the net component, which is a radio, an OTA flash writer
// and a TLS stack, none of which belong in an emulator. What it needs of them is small,
// and stands in here: the watch is always on Wi-Fi, and the pull client is a little
// state machine driven by EMU_OTA - unset or "uptodate": the site has nothing new;
// "available": it offers 0.9.10 (and the download can be started, running 4 s to done);
// "ready": a download is already waiting to install; "failed": the check fails.
// EMU_USB, through the PMU above, says whether the cable is in - which is what the
// install gate looks at.
namespace console {
bool safeMode() { return false; }
const char *firmwareVersion() { return "0.9.9"; }
}  // namespace console

extern "C" {

void esp_restart(void) { emu_log_line('W', "emu", "esp_restart called - the emulator carries on"); }

net_state_t net_state(void) { return NET_CONNECTED; }
bool net_saved_ssid(char *out, size_t len) { snprintf(out, len, "EMU-NET"); return true; }
const char *net_ip(void) { return "192.168.1.42"; }
const char *net_device_key(void) { return "0123456789abcdef"; }
esp_err_t ota_server_start(void) { return ESP_OK; }
void ota_get_status(ota_status_t *out) { out->state = OTA_IDLE; }
bool net_join_saved(uint32_t) { return true; }
bool net_join(const char *, const char *, uint32_t) { return true; }
int net_scan(net_ap_t *, int) { return 0; }
bool net_enabled(void) { return true; }
void net_set_enabled(bool) {}

static ota_pull_status_t s_pull;
static int64_t s_pull_state_us;
static char s_pull_mode[16];

static void pull_state(ota_pull_state_t st)
{
    s_pull.state = st;
    s_pull_state_us = esp_timer_get_time();
    if (st == OTA_PULL_DOWNLOADING) {
        s_pull.received = 0;
        s_pull.total = s_pull.manifest.size;
    }
}

static void pull_manifest(ota_manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->version, sizeof(m->version), "0.9.10");
    snprintf(m->build, sizeof(m->build), "fedcba9876543210");
    memset(m->sha256, 'a', 64);
    m->sha256[64] = 0;
    snprintf(m->url, sizeof(m->url), "http://127.0.0.1:8080/firmware/gyrotron.bin");
    snprintf(m->changelog, sizeof(m->changelog), "Demo update for the emulator.");
    m->size = 1310720;   // 1.25 MB, so the ring has something to crawl around
}

void ota_pull_init(void)
{
    const char *mode = std::getenv("EMU_OTA");
    snprintf(s_pull_mode, sizeof(s_pull_mode), "%s", mode ? mode : "");
    if (std::strcmp(s_pull_mode, "ready") == 0) {
        pull_manifest(&s_pull.manifest);
        pull_state(OTA_PULL_READY);
    }
}

void ota_pull_on_ip(void) {}
void ota_pull_on_time(void) {}

bool ota_pull_pending(char *version, size_t len)
{
    const bool on = s_pull.state == OTA_PULL_AVAILABLE || s_pull.state == OTA_PULL_DOWNLOADING ||
                    s_pull.state == OTA_PULL_READY;
    if (on && version) snprintf(version, len, "%s", s_pull.manifest.version);
    return on;
}

bool ota_pull_active(void)
{
    return s_pull.state == OTA_PULL_CHECKING || s_pull.state == OTA_PULL_DOWNLOADING ||
           s_pull.state == OTA_PULL_APPLYING;
}

void ota_pull_get_status(ota_pull_status_t *out)
{
    const int64_t held_us = 800000;   // how long "..." lasts, so the state is seeable
    if (s_pull.state == OTA_PULL_CHECKING && esp_timer_get_time() - s_pull_state_us > held_us) {
        if (std::strcmp(s_pull_mode, "failed") == 0) {
            snprintf(s_pull.error, sizeof(s_pull.error), "no manifest on the site (404)");
            pull_state(OTA_PULL_FAILED);
        } else if (s_pull_mode[0] == 0 || std::strcmp(s_pull_mode, "uptodate") == 0) {
            pull_state(OTA_PULL_UPTODATE);
        } else {
            pull_manifest(&s_pull.manifest);
            pull_state(OTA_PULL_AVAILABLE);
        }
    }
    if (s_pull.state == OTA_PULL_DOWNLOADING) {
        const int64_t dt = esp_timer_get_time() - s_pull_state_us;
        s_pull.received = (uint32_t)((uint64_t)s_pull.total * dt / 4000000);
        if (s_pull.received > s_pull.total) s_pull.received = s_pull.total;
        if (dt >= 4000000) pull_state(OTA_PULL_READY);
    }
    *out = s_pull;
}

bool ota_pull_start_check(void)
{
    if (ota_pull_active()) return false;
    s_pull.error[0] = 0;
    pull_state(OTA_PULL_CHECKING);
    return true;
}

bool ota_pull_start_download(void)
{
    if (ota_pull_active() || s_pull.state != OTA_PULL_AVAILABLE) return false;
    s_pull.error[0] = 0;
    pull_state(OTA_PULL_DOWNLOADING);
    return true;
}

void ota_pull_cancel(void)
{
    if (s_pull.state == OTA_PULL_DOWNLOADING) {
        snprintf(s_pull.error, sizeof(s_pull.error), "cancelled");
        pull_state(OTA_PULL_AVAILABLE);
    }
}

bool ota_power_ok(void) { return pmu_usb_power() || pmu_charging(); }

esp_err_t ota_pull_apply(void)
{
    if (s_pull.state != OTA_PULL_READY || !ota_power_ok()) return ESP_ERR_INVALID_STATE;
    pull_state(OTA_PULL_APPLYING);
    emu_log_line('I', "ota", "EMU: boot slot switched; a real watch restarts here");
    return ESP_OK;
}

}  // extern "C"
#endif  // EMU_SCREEN_UPDATE

// ------------------------------------------------------------------ the store screen

#ifdef EMU_SCREEN_STORE
// The store stands on the net component's catalog fetcher and on the loader/storage
// pair, none of which belong in an emulator. The fakes: the watch is always on Wi-Fi,
// and the catalog fetcher is a small state machine driven by EMU_STORE - a local copy
// of the catalog deploy tree (the index at games/index-watch.json, the packages at the
// catalog-relative urls the index names). An index fetch reads the file in after a
// short delay (so the FETCHING screen is seeable); a package "download" copies it
// into Games/ over about a second, progress on the ring. Installs are remembered for
// the session, so a scripted run can walk list -> detail -> install -> INSTALLED.
#include <cctype>

extern "C" {

net_state_t net_state(void) { return NET_CONNECTED; }
bool net_saved_ssid(char *out, size_t len) { snprintf(out, len, "EMU-NET"); return true; }
const char *net_ip(void) { return "192.168.1.42"; }
bool net_join_saved(uint32_t) { return true; }
bool net_join(const char *, const char *, uint32_t) { return true; }
int net_scan(net_ap_t *out, int max)
{
    // Two networks, so the setup screen has something to show if a script opens it.
    if (max > 0) { snprintf(out[0].ssid, sizeof(out[0].ssid), "EMU-NET"); out[0].rssi = -40; out[0].secure = true; }
    if (max > 1) { snprintf(out[1].ssid, sizeof(out[1].ssid), "NEIGHBOUR"); out[1].rssi = -65; out[1].secure = true; }
    return max > 1 ? 2 : 1;
}
bool net_enabled(void) { return true; }
void net_set_enabled(bool) {}
void storage_changed(void) {}

bool storage_free_bytes(uint32_t *total, uint32_t *free_bytes)
{
    *total = 16u * 1024 * 1024;
    *free_bytes = 8u * 1024 * 1024;
    return true;
}

static char s_store_dir[256];
static const char *store_dir()
{
    const char *v = std::getenv("EMU_STORE");
    if (!v || !v[0]) v = ".";
    snprintf(s_store_dir, sizeof(s_store_dir), "%s", v);
    return s_store_dir;
}

// ---------------- the fake catalog fetcher

static store_status_t s_index_st, s_pkg_st;
static char *s_body = nullptr;   // holds the last fetched index until the next fetch
static int64_t s_started_us;
static char s_pkg_from[512], s_pkg_to[160];
static uint32_t s_pkg_len;

bool store_index_fetch(void)
{
    if (s_index_st.state == STORE_FETCHING) return false;
    free(s_body);
    s_body = nullptr;
    // The same relative layout the real host serves under /otadb/: the index at
    // games/index-watch.json, packages at the catalog-relative urls the index names.
    FILE *f = fopen((std::string(store_dir()) + "/games/index-watch.json").c_str(), "rb");
    if (!f) {
        snprintf(s_index_st.error, sizeof(s_index_st.error), "no catalog on the site (404)");
        s_index_st.state = STORE_FAILED;
        return true;
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_body = static_cast<char *>(malloc(len + 1));
    const size_t got = s_body ? fread(s_body, 1, len, f) : 0;
    fclose(f);
    if (!s_body || got != (size_t)len) {
        free(s_body);
        s_body = nullptr;
        snprintf(s_index_st.error, sizeof(s_index_st.error), "empty reply from the site");
        s_index_st.state = STORE_FAILED;
        return true;
    }
    s_body[len] = 0;
    s_index_st.state = STORE_FETCHING;
    s_index_st.error[0] = 0;
    s_started_us = esp_timer_get_time();
    return true;
}

const char *store_index_body(void) { return s_body; }

static void poll_index()
{
    if (s_index_st.state != STORE_FETCHING) return;
    if (esp_timer_get_time() - s_started_us < 600000) return;   // seeable, then done
    s_index_st.state = STORE_OK;
    s_index_st.received = s_index_st.total = (uint32_t)strlen(s_body);
    // The real fetcher logs the same two lines (game_store.c); scripted runs wait on them.
    emu_log_line('I', "store", "index fetched (%u bytes)", s_index_st.total);
}

void store_index_status(store_status_t *out)
{
    poll_index();
    *out = s_index_st;
}

bool store_package_fetch(const char *url, const char *dest_path, const char *sha256_hex, uint32_t size)
{
    (void)sha256_hex;
    if (s_pkg_st.state == STORE_FETCHING) return false;
    snprintf(s_pkg_from, sizeof(s_pkg_from), "%s/%s", store_dir(), url);
    snprintf(s_pkg_to, sizeof(s_pkg_to), "%s.part", dest_path);
    FILE *f = fopen(s_pkg_from, "rb");
    if (!f) {
        snprintf(s_pkg_st.error, sizeof(s_pkg_st.error), "the package is not on the site (404)");
        s_pkg_st.state = STORE_FAILED;
        return true;
    }
    fseek(f, 0, SEEK_END);
    s_pkg_len = (uint32_t)ftell(f);
    fclose(f);
    (void)size;
    s_pkg_st.state = STORE_FETCHING;
    s_pkg_st.error[0] = 0;
    s_pkg_st.total = s_pkg_len;
    s_pkg_st.received = 0;
    s_started_us = esp_timer_get_time();
    return true;
}

void store_package_status(store_status_t *out)
{
    if (s_pkg_st.state == STORE_FETCHING) {
        const int64_t dt = esp_timer_get_time() - s_started_us;
        s_pkg_st.received = (uint32_t)((uint64_t)s_pkg_len * dt / 1500000);
        if (s_pkg_st.received >= s_pkg_len) {
            // "Finished": copy the bytes into Games/, then the rename the loader sees.
            FILE *in = fopen(s_pkg_from, "rb");
            FILE *dst = fopen(s_pkg_to, "wb");
            if (in && dst) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, dst);
            }
            if (in) fclose(in);
            if (dst) fclose(dst);
            char final_path[160];
            snprintf(final_path, sizeof(final_path), "%s", s_pkg_to);
            final_path[strlen(final_path) - 5] = 0;   // drop ".part"
            remove(final_path);
            rename(s_pkg_to, final_path);
            s_pkg_st.received = s_pkg_len;
            s_pkg_st.state = STORE_OK;
            emu_log_line('I', "store", "%s installed (%u KB)", final_path, s_pkg_len / 1024);
        }
    }
    *out = s_pkg_st;
}

void store_fetch_cancel(void)
{
    if (s_pkg_st.state == STORE_FETCHING) {
        snprintf(s_pkg_st.error, sizeof(s_pkg_st.error), "cancelled");
        s_pkg_st.state = STORE_FAILED;
    }
    if (s_index_st.state == STORE_FETCHING) s_index_st.state = STORE_FAILED;
}

bool store_fetch_active(void) { return s_index_st.state == STORE_FETCHING || s_pkg_st.state == STORE_FETCHING; }

// ---------------- a stand-in for the loader's games-folder view

// The store's view of what is installed: the packages the fake store has put in
// Games/ this session (the fake fetcher writes real files there), so a scripted run
// sees INSTALLED after an install. MSVC has no dirent, so no directory scan - the
// list grows from the fetcher's own record.
static loader_entry_t s_installed[LOADER_MAX_GAMES];
static int s_installed_n;

static void add_installed(const char *id)
{
    if (s_installed_n >= LOADER_MAX_GAMES) return;
    for (int i = 0; i < s_installed_n; i++)
        if (strcmp(s_installed[i].id, id) == 0) return;
    loader_entry_t &e = s_installed[s_installed_n];
    memset(&e, 0, sizeof(e));
    snprintf(e.path, sizeof(e.path), "Games/%s.tat", id);
    snprintf(e.id, sizeof(e.id), "%s", id);   // the real loader reports the package's own id, lowercase
    snprintf(e.name, sizeof(e.name), "%s", id);
    for (char *c = e.name; *c; c++) *c = (char)toupper((unsigned char)*c);
    e.version = 1;
    e.api_major = TAT_API_MAJOR;
    e.api_minor = TAT_API_MINOR;
    e.size = s_pkg_len;
    s_installed_n++;
}

int loader_scan(loader_entry_t *out, int max)
{
    if (s_pkg_st.state == STORE_OK) {
        const char *base = strrchr(s_pkg_to, '/');
        base = base ? base + 1 : s_pkg_to;
        const char *dot = strstr(base, ".tat.part");   // strrchr would find ".part"'s dot first
        if (dot) add_installed(std::string(base, dot - base).c_str());
        s_pkg_st.state = STORE_OK;   // stay; add_installed only acts on a fresh install's path
    }
    const int n = std::min(s_installed_n, max);
    for (int i = 0; i < n; i++) out[i] = s_installed[i];
    return n;
}

const char *loader_games_dir(void) { return "Games"; }

}  // extern "C"
#endif  // EMU_SCREEN_STORE

// ------------------------------------------------------------------ NVS, in a file

struct NvsEntry {
    std::string ns, key;
    int kind;   // 0 i32, 1 u8, 2 u16, 3 blob
    int32_t value;
    std::vector<unsigned char> blob;
};
static std::vector<NvsEntry> s_nvs;
static std::vector<std::string> s_open;   // handle - 1 indexes the namespace it was opened on
static std::string s_nvs_path;

static NvsEntry *nvs_find(nvs_handle_t h, const char *key)
{
    if (h == 0 || h > s_open.size()) return nullptr;
    for (auto &e : s_nvs)
        if (e.ns == s_open[h - 1] && e.key == key) return &e;
    return nullptr;
}

static void nvs_write_file()
{
    if (s_nvs_path.empty()) return;
    FILE *f = std::fopen(s_nvs_path.c_str(), "w");
    if (!f) return;
    for (const auto &e : s_nvs) {
        if (e.kind == 3) {
            std::fprintf(f, "%s %s blob", e.ns.c_str(), e.key.c_str());
            for (unsigned char b : e.blob) std::fprintf(f, " %02x", b);
            std::fprintf(f, "\n");
        } else {
            std::fprintf(f, "%s %s %s %d\n", e.ns.c_str(), e.key.c_str(), e.kind == 0 ? "i32" : e.kind == 1 ? "u8" : "u16",
                         (int)e.value);
        }
    }
    std::fclose(f);
}

static void nvs_read_file()
{
    s_nvs.clear();
    FILE *f = s_nvs_path.empty() ? nullptr : std::fopen(s_nvs_path.c_str(), "r");
    if (!f) return;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        char ns[64], key[64], kind[16];
        int used = 0;
        if (std::sscanf(line, "%63s %63s %15s%n", ns, key, kind, &used) != 3) continue;
        NvsEntry e{ns, key, 0, 0, {}};
        if (!std::strcmp(kind, "blob")) {
            e.kind = 3;
            const char *p = line + used;
            unsigned b;
            int n;
            while (std::sscanf(p, " %2x%n", &b, &n) == 1) {
                e.blob.push_back((unsigned char)b);
                p += n;
            }
        } else {
            e.kind = !std::strcmp(kind, "u8") ? 1 : !std::strcmp(kind, "u16") ? 2 : 0;
            e.value = std::atoi(line + used);
        }
        s_nvs.push_back(e);
    }
    std::fclose(f);
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t, nvs_handle_t *out)
{
    s_open.push_back(ns ? ns : "");
    *out = (nvs_handle_t)s_open.size();
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t)
{
    nvs_write_file();
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    for (size_t i = 0; i < s_nvs.size(); i++)
        if (h && h <= s_open.size() && s_nvs[i].ns == s_open[h - 1] && s_nvs[i].key == key) {
            s_nvs.erase(s_nvs.begin() + (long)i);
            return ESP_OK;
        }
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t nvs_get(nvs_handle_t h, const char *key, int kind, int32_t *v)
{
    NvsEntry *e = nvs_find(h, key);
    if (!e) return ESP_ERR_NOT_FOUND;
    if (e->kind != kind) return ESP_ERR_NVS_TYPE_MISMATCH;
    *v = e->value;
    return ESP_OK;
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v) { return nvs_get(h, key, 0, v); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v)
{
    int32_t x;
    const esp_err_t r = nvs_get(h, key, 1, &x);
    if (r == ESP_OK) *v = (uint8_t)x;
    return r;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v)
{
    int32_t x;
    const esp_err_t r = nvs_get(h, key, 2, &x);
    if (r == ESP_OK) *v = (uint16_t)x;
    return r;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v)
{
    NvsEntry *e = nvs_find(h, key);
    if (e && e->kind != 0) return ESP_ERR_NVS_TYPE_MISMATCH;   // the real one refuses a change of width
    if (!e) {
        if (h == 0 || h > s_open.size()) return ESP_FAIL;
        s_nvs.push_back({s_open[h - 1], key, 0, v, {}});
    } else {
        e->value = v;
    }
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)
{
    NvsEntry *e = nvs_find(h, key);
    if (e && e->kind != 1) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (!e) {
        if (h == 0 || h > s_open.size()) return ESP_FAIL;
        s_nvs.push_back({s_open[h - 1], key, 1, v, {}});
    } else {
        e->value = v;
    }
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    NvsEntry *e = nvs_find(h, key);
    // The real NVS's own code for a missing key, not the generic ESP_ERR_NOT_FOUND this
    // used to return: tiltcal_store tells "never calibrated" from "unreadable" apart by
    // it, and a stub that answers with different codes than the part it stands in for
    // sends the diagnostic down the wrong branch.
    if (!e || e->kind != 3) return ESP_ERR_NVS_NOT_FOUND;
    if (out) {
        if (*len < e->blob.size()) return ESP_FAIL;
        std::memcpy(out, e->blob.data(), e->blob.size());
    }
    *len = e->blob.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    NvsEntry *e = nvs_find(h, key);
    if (!e) {
        if (h == 0 || h > s_open.size()) return ESP_FAIL;
        s_nvs.push_back({s_open[h - 1], key, 3, 0, {}});
        e = &s_nvs.back();
    }
    e->kind = 3;
    e->blob.assign((const unsigned char *)data, (const unsigned char *)data + len);
    return ESP_OK;
}

// ------------------------------------------------------------------ the panel

// What would have gone down the wire. A canvas game streams its picture in bands, just as
// it does to the real panel; a menu draws into the framebuffer and that is copied instead.
static wc::Color s_frame[wc::Gfx::W * wc::Gfx::H];
static bool s_streamed = false;

namespace wc {

bool Presenter::init(int, int) { return true; }

void Presenter::presentBands(const BandFill &fill)
{
    static Color band[Gfx::W * 34];
    const int rows_max = 34;
    for (int y = 0; y < Gfx::H; y += rows_max) {
        const int rows = std::min(rows_max, Gfx::H - y);
        int x0 = Gfx::W, x1 = 0;
        for (int b = y / Gfx::BAND_H; b <= (y + rows - 1) / Gfx::BAND_H; b++) {
            int vx0, vx1;
            Gfx::visibleSpan(b, vx0, vx1);
            x0 = std::min(x0, vx0);
            x1 = std::max(x1, vx1);
        }
        if (x1 <= x0) continue;
        fill(y, rows, x0, x1 - x0, band);
        for (int r = 0; r < rows; r++)
            std::memcpy(s_frame + (y + r) * Gfx::W + x0, band + r * (x1 - x0), size_t(x1 - x0) * sizeof(Color));
    }
    s_streamed = true;
    streamed_ = true;
}

// A game that has nothing new to show simply does not present, and the panel goes on showing
// what it had - so the framebuffer is only copied when it is the thing being looked at,
// which for a game is when its pause menu is open.
void Presenter::present(Gfx &gfx)
{
#ifdef EMU_SCREEN
    const bool looked_at = true;   // a console screen draws nowhere else
#else
    const bool looked_at = tat::api()->menu_is_open();
#endif
    if (!streamed_ && looked_at) std::memcpy(s_frame, gfx.pixels(), sizeof(s_frame));
    streamed_ = false;
}

void Presenter::flush() {}

// The engine's own input is a task reading I2C. Here it is whatever the front end says:
// emu_input stores the raw sample - in BOARD axes, the front end mounts its synthetic
// chip the way the real board's is mounted - and snapshot runs the same pipeline the
// task runs on the watch - map first, correction after - so EMU_ACC_* fault injection
// and the whole calibration flow exercise the real math, not a lookalike. The default
// record is the uncalibrated watch's: the guess map runs until a calibration is saved.
// The state sits at file scope: emu_input is a plain extern "C" and cannot see into wc.
static Tilt s_raw;
static TiltCal s_cal;   // has_map = 0 -> applyPipeline falls back to guessAxMap()

bool Input::init() { return true; }
void Input::pause(bool) {}
void Input::setCalibration(const TiltCal &cal) { s_cal = cal; }
TiltCal Input::calibration() { return s_cal; }
Tilt Input::rawTilt() { return s_raw; }
void Input::snapshot(InputState &out) { cal::applyPipeline(s_cal, s_raw, out.tilt); }

}  // namespace wc

// The one door into the engine that engine.h leaves open for this.
namespace wc {
struct EngineEmulator {
    static InputState &input(Engine &e) { return e.input_state_; }
    static void snapshot_input(Engine &e, InputState &out) { e.input_.snapshot(out); }
    static bool wants_home(Engine &e) { return e.pending_ != nullptr; }
    static void set_home(Engine &e, Game &home)
    {
        e.home_ = &home;
        e.pending_ = nullptr;
    }
};
}  // namespace wc
using wc::EngineEmulator;

// ------------------------------------------------------------------ what the front end calls

struct HomeMarker : wc::Game {
    void update(wc::Engine &, float) override {}
    void draw(wc::Engine &, wc::Gfx &) override {}
};

#ifdef EMU_SCREEN_BOOT
// The boot animation, as BootSplash plays it: two spins, a stretch of "setting up" as a new
// watch would show, and round again.
struct BootScreen : wc::Game {
    console::BootAnim anim;
    float t = 0;
    void begin(wc::Engine &) override { anim.init(); }
    void update(wc::Engine &, float dt) override { t += dt; }
    void draw(wc::Engine &e, wc::Gfx &) override
    {
        const float lap = console::BootAnim::SPIN_S * 2 + 2.0f, at = std::fmod(t, lap);
        if (at < console::BootAnim::SPIN_S * 2) anim.spin(e.presenter(), at);
        else anim.waiting(e.presenter(), "SETTING UP", "SKATER GIRLZ", int((at - console::BootAnim::SPIN_S * 2) * 5), 10);
    }
};
#endif

static wc::Engine s_engine;
static HomeMarker s_home;
#ifdef EMU_SCREEN
static wc::Game *s_game;
#else
static tat::HostedGame *s_game;
#endif
static std::vector<std::vector<unsigned char>> s_asset_bytes;
static std::vector<std::string> s_asset_names;

static struct {
    bool down;
    int x, y;
    uint32_t held;
    float ax, ay, az, gx, gy, gz;   // board axes, like s_raw
    float rx, ry, rz;               // board-frame turn over the frame, degrees
    float rdt;                      // seconds the turn spans
} s_in = {false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, s_prev = s_in;   // overwritten by the first step
static int64_t s_btn_down_us[8];

#ifdef EMU_SCREEN
EMU_API const char *emu_game_id(void) { return "screen"; }
EMU_API const char *emu_game_name(void) { return "GYRO-TRON"; }
#else
EMU_API const char *emu_game_id(void) { return tat_game.id; }
EMU_API const char *emu_game_name(void) { return tat_game.name; }
#endif

// Before emu_begin: each file in the game's assets folder, by the name the game asks for.
EMU_API void emu_add_asset(const char *name, const unsigned char *data, int len)
{
    s_asset_names.emplace_back(name);
    s_asset_bytes.emplace_back(data, data + len);
}

EMU_API void emu_options(const char *save_path, unsigned seed, int quiet)
{
    s_nvs_path = save_path ? save_path : "";
    if (seed) s_rng = seed;
    s_quiet = quiet != 0;
}

EMU_API int emu_begin(void)
{
    nvs_read_file();
#ifdef EMU_SCREEN
    if (!s_engine.gfx().init()) return 0;
    EngineEmulator::set_home(s_engine, s_home);
#ifdef EMU_SCREEN_BOOT
    s_game = new BootScreen;
#elif defined(EMU_SCREEN_HOME)
    s_game = new console::Launcher(k_apps, int(sizeof(k_apps) / sizeof(k_apps[0])));
#elif defined(EMU_SCREEN_UPDATE)
    ota_pull_init();
    s_game = new console::UpdateApp(&s_home);
#elif defined(EMU_SCREEN_STORE)
    s_game = new console::StoreApp(&s_home);
#elif defined(EMU_SCREEN_STORE)
    s_game = new console::StoreApp(&s_home);
#elif defined(EMU_SCREEN_CAL)
    console::loadTiltCalibration();   // also hands the refresher its save hook
    s_game = new console::CalibrateApp(&s_home);
#else
    s_game = new console::PowerMenu;
#endif
#ifdef EMU_SCREEN_HOLD
    EngineEmulator::input(s_engine).held = wc::BTN_B;   // as the engine shows it: PWR still down
#endif
    s_game->begin(s_engine);
    s_game->enter(s_engine);
    return 1;
#else
    for (size_t i = 0; i < s_asset_names.size() && i < 31; i++) {
        emu_set_asset((int)i, s_asset_names[i].c_str(), s_asset_bytes[i].data(),
                      s_asset_bytes[i].data() + s_asset_bytes[i].size());
    }
    if (tat_game.asset_count > (int)s_asset_names.size())
        emu_log_line('W', "emu", "%s declares %d assets and its folder has %d", tat_game.id, tat_game.asset_count,
                     (int)s_asset_names.size());
    if (!s_engine.gfx().init()) return 0;
    EngineEmulator::set_home(s_engine, s_home);
    s_game = new tat::HostedGame(tat_game);
    s_game->begin(s_engine);
    s_game->enter(s_engine);
    return 1;
#endif
}

// Held buttons are a mask of TAT_BTN_*; touch is in screen pixels; tilt is gravity in g
// and `g*` the turn rate in degrees per second, both in BOARD axes - what the chip on
// the real board would report, before the firmware's axis map. `r*`/rdt carry the same
// frame's integrated rotation in board axes: the front end derives it from the pose
// change itself, so it is exact even for a scripted pose jump. The front end mounts its
// synthetic chip the way the real one is mounted (see Console.MOUNT in emu.py), so the
// map pipeline, and the calibration flow that measures it, run against a real mount.
EMU_API void emu_input(int touch_down, int x, int y, unsigned held, float ax, float ay, float az,
                       float gx, float gy, float gz, float rx, float ry, float rz, float rdt)
{
    s_in.down = touch_down != 0;
    if (touch_down) s_in.x = x, s_in.y = y;
    s_in.held = held;
    s_in.ax = ax, s_in.ay = ay, s_in.az = az;
    s_in.gx = gx, s_in.gy = gy, s_in.gz = gz;
    s_in.rx = rx, s_in.ry = ry, s_in.rz = rz;
    s_in.rdt = rdt;
    wc::s_raw = {ax, ay, az, gx, gy, gz, rx, ry, rz, rdt};   // what the input task would have stored
}

// One frame: what the engine's loop does, without the engine's loop. Returns 1 if the game
// asked to go home.
EMU_API int emu_step(float dt)
{
    s_now_us += (int64_t)(dt * 1e6f);
    wc::InputState &in = EngineEmulator::input(s_engine);
    in = wc::InputState{};
    in.touch.down = s_in.down;
    in.touch.pressed = s_in.down && !s_prev.down;
    in.touch.released = !s_in.down && s_prev.down;
    in.touch.x = s_in.x;
    in.touch.y = s_in.y;
    in.touch.t_us = s_now_us;
    in.held = s_in.held;
    in.pressed = s_in.held & ~s_prev.held;
    in.released = ~s_in.held & s_prev.held;
    for (int b = 0; b < 8; b++) {
        const uint32_t bit = 1u << b;
        if (in.pressed & bit) s_btn_down_us[b] = s_now_us;
        if ((in.released & bit) && s_now_us - s_btn_down_us[b] < 600000) in.clicked |= bit;
        if ((in.held & bit) && s_now_us - s_btn_down_us[b] >= 600000 && s_now_us - s_btn_down_us[b] - (int64_t)(dt * 1e6f) < 600000)
            in.long_press |= bit;
    }
    in.tilt.ax = s_in.ax;
    in.tilt.ay = s_in.ay;
    in.tilt.az = s_in.az;
    in.tilt.gx = s_in.gx;
    in.tilt.gy = s_in.gy;
    in.tilt.gz = s_in.gz;
    EngineEmulator::snapshot_input(s_engine, in);   // the same map + correction the watch's task applies
#ifdef EMU_SCREEN_CAL
    wc::cal::autocal().feed(wc::s_raw);   // the stand-in for the input task's refresher feed
#endif
    s_prev = s_in;

    s_streamed = false;
    s_game->update(s_engine, dt);
    s_game->draw(s_engine, s_engine.gfx());
    s_engine.presenter().present(s_engine.gfx());
    return EngineEmulator::wants_home(s_engine) ? 1 : 0;
}

// The screen as 466 x 466 RGB, three bytes a pixel, with the corners the round panel does
// not have painted `corner` so a picture of it looks like the watch.
EMU_API void emu_frame(unsigned char *rgb, int corner_r, int corner_g, int corner_b)
{
    const int W = wc::Gfx::W, H = wc::Gfx::H;
    const float c = (W - 1) * 0.5f, r2 = (W * 0.5f) * (W * 0.5f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned char *o = rgb + (y * W + x) * 3;
            const float dx = x - c, dy = y - c;
            if (dx * dx + dy * dy > r2) {
                o[0] = (unsigned char)corner_r, o[1] = (unsigned char)corner_g, o[2] = (unsigned char)corner_b;
                continue;
            }
            wc::Color v = s_frame[y * W + x];
            v = wc::Color((v >> 8) | (v << 8));   // the framebuffer is kept in the panel's byte order
            o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
            o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
            o[2] = (unsigned char)((v & 31) * 255 / 31);
        }
}

EMU_API int emu_tones(void) { return wc::audio::s_tones; }
EMU_API int emu_keep_awake(void) { return s_game && s_game->keepAwake() ? 1 : 0; }

// What the console asks for on the way to deep sleep, and what it hands back on the wake -
// exposed so a host test can walk a game through a save/load round trip.
EMU_API int emu_state_save(unsigned char *buf, unsigned max)
{
    return s_game ? (int)s_game->saveState(buf, max) : 0;
}
EMU_API bool emu_state_load(const unsigned char *buf, unsigned len)
{
    return s_game && s_game->loadState(buf, len);
}

EMU_API void emu_end(void)
{
    delete s_game;   // tells the game it is going, and reports whatever it left behind
    s_game = nullptr;
    nvs_write_file();
}
