// Wi-Fi + over-the-air firmware updates.
//
// Wi-Fi is off unless the user turns it on in Settings. When enabled it connects
// in the background at boot and keeps reconnecting (radio power-save on, so it
// costs games as little as possible).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_HOSTNAME "gyro-tron"   // reachable as gyro-tron.local

// The epoch from which the system clock is believed: anything older is the 1970 the
// chip booted with, not a time anyone set or synced. Both the cached-time restore and
// the update check's "is the clock real enough" test against this one number (the
// clock face has its own copy of it).
#define NET_CLOCK_REAL_EPOCH 1700000000LL   // 2023-11-14

// ---------------------------------------------------------------- Wi-Fi
typedef enum {
    NET_OFF,          // disabled by the user
    NET_NO_NETWORK,   // enabled but no saved network
    NET_CONNECTING,   // trying / reconnecting
    NET_CONNECTED,
    NET_FAILED,       // saved network rejected us (wrong password?) or can't be found
} net_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_ap_t;

// Persisted on/off switch. Turning on starts a background connect.
bool net_enabled(void);
void net_set_enabled(bool on);
// Call once at boot, right after NVS is up: puts the last time NTP knew about back into
// the system clock, so a full power-off costs at most the minutes since the last save
// instead of landing in 1970 until Wi-Fi connects.
void net_restore_cached_time(void);
// Also write the current system time into that cache - for when it was set by hand
// (clock app) rather than from NTP.
void net_time_cache_save(void);
// Call once at boot: connects in the background if enabled and a network is saved.
void net_start(void);
// Safe mode: bring Wi-Fi up with the saved network even if the user switched it off.
void net_start_forced(void);
net_state_t net_state(void);
// Minutes east of UTC for this network's location (from ip-api.com, once per boot,
// daylight saving included). False until it's known.
bool net_tz_offset_min(int *minutes);
// Around console sleep: radio off, then back on and reconnecting if it was enabled.
void net_suspend(void);
void net_resume(void);
const char *net_ip(void);

// Saved network name, if any (also imports Wi-Fi a PeakPal firmware left in NVS).
bool net_saved_ssid(char *out, size_t len);
// Blocking join used by the setup screen. On success the network is saved,
// Wi-Fi is switched on, and it stays connected in the background.
bool net_join(const char *ssid, const char *pass, uint32_t timeout_ms);
// Blocking join of the saved network (update mode).
bool net_join_saved(uint32_t timeout_ms);
// Blocking scan, strongest first, duplicates and hidden networks removed.
int net_scan(net_ap_t *out, int max);

// ---------------------------------------------------------------- OTA server
typedef enum { OTA_IDLE, OTA_RECEIVING, OTA_DONE, OTA_FAILED } ota_state_t;
typedef struct {
    ota_state_t state;
    uint32_t received;
    uint32_t total;
    char error[48];
} ota_status_t;

// HTTP on port 80: GET / (upload page), GET /status (JSON), GET /log (recent log
// lines), GET /reboot, POST /update (raw .bin body), GET /screen, GET /input,
// GET /apps, GET /tilt, POST /script (remote debugging, docs/reference/remote_debug.md)
esp_err_t ota_server_start(void);

// Every endpoint except /status needs this key, as ?key=... or an X-Tat-Key header.
// Without it anyone on the same network could flash the watch, drive it or watch its
// screen. It is made once on first use and kept in NVS; the watch shows it on the update
// screen, and hands it over the USB link, where physical access already implies trust.
#define NET_KEY_LEN 17   // 16 hex characters and a terminator
const char *net_device_key(void);
// Keep the most recent log output in memory so it can be read over Wi-Fi at /log.
// Call first thing at boot.
void net_log_capture_start(void);
// Copy the recent log into out (up to max bytes), oldest first. Returns how many it
// wrote, 0 when capture is off. What LINK_LOG and GET /log both serve.
size_t net_log_tail(uint8_t *out, size_t max);
// Extra fields for /status, e.g. "\"safe\":true,\"crash\":\"...\"" (no braces).
void net_status_set_extra(const char *json_fields);
// GET /screen returns a picture of the display. fmt picks the format: "png" (what a
// browser wants, slow to encode), "raw" (the whole frame as RGB565, little-endian,
// no encoding), or "delta" (only what changed since the previous delta; GTD1 format,
// docs/reference/remote_debug.md). The hook must return a malloc'd buffer the server
// frees, and its size - 0 on failure.
typedef size_t (*net_screen_fn)(const char *fmt, uint8_t **out);
void net_set_screen_hook(net_screen_fn fn);

// GET /apps: the hook writes a JSON array of the carousel's apps into out, in the
// order /input?app=N indexes them - {"i":1,"id":"breakout","name":"BREAKOUT"},...
typedef void (*net_apps_fn)(char *out, size_t len);
void net_set_apps_hook(net_apps_fn fn);

// GET /input?<query>: remote control for testing; the hook gets the raw query string.
typedef bool (*net_control_fn)(const char *query);
void net_set_control_hook(net_control_fn fn);

// POST /script: the hook runs the body's steps (the same words as the emulator's
// scripts, docs/reference/remote_debug.md) and calls emit() per record: type 1 is a
// screen frame (RGB565, little-endian), 2 a text note, 3 an error. The server frames
// the records; the hook only says what they are.
typedef void (*net_script_emit)(uint8_t type, const void *data, size_t len);
typedef bool (*net_script_fn)(const char *body, net_script_emit emit);
void net_set_script_hook(net_script_fn fn);

// GET /tilt: what the motion sensor is reading, as text. For working out whether a
// complaint about the tilt is the sensor, the calibration, or the game.
typedef void (*net_tilt_fn)(char *out, size_t len);
void net_set_tilt_hook(net_tilt_fn fn);
// While this returns true, reboots requested over HTTP (after an update, /reboot)
// are postponed: restarting while a computer has the drive open corrupts it.
// True while an upload is running, or within two minutes of any request to the
// watch's web server: the console stays awake for as long as someone is working with it.
bool net_busy(void);
bool net_transfer_active(void);
void ota_get_status(ota_status_t *out);

// ---------------------------------------------------------------- pull updates (from the site)
// The watch checks the release site for a newer build, downloads it into the idle app slot
// over Wi-Fi, and switches slots + restarts only while plugged in (docs/reference/OTA.md). Checking
// and downloading are fine on battery; the install is the one moment a dead battery could
// turn an update into a recovery, and a cable takes that moment off the table.
//
// The site address is one constant on purpose: it ships in every build, and a manifest that
// asks the watch to fetch its image from somewhere else is refused. Plain HTTP on purpose:
// every fetch is hash-checked (SHA-256 in the manifest / the catalog entry), so the
// transport adds no integrity the hashes don't already give - only a certificate story
// the watch's root store would have to believe.
#define TAT_OTA_BASE_URL "http://tat.secstant.ru"   // serves /firmware/version.json (see site/)

typedef struct {
    char version[32];    // "0.9.10" - for the UI
    char build[17];      // 16 hex: the build's ELF-hash prefix, the id /status calls "sha"
    char sha256[65];     // SHA-256 of the whole .bin, checked while it streams in
    char url[192];       // where the .bin is; absolute, or relative to TAT_OTA_BASE_URL
    char changelog[192];
    uint32_t size;
} ota_manifest_t;

typedef enum {
    OTA_PULL_IDLE = 0,     // never checked, or nothing pending
    OTA_PULL_CHECKING,     // asking the site for version.json
    OTA_PULL_UPTODATE,
    OTA_PULL_AVAILABLE,    // the site has a newer build
    OTA_PULL_DOWNLOADING,
    OTA_PULL_READY,        // downloaded and verified; waiting for USB power to install
    OTA_PULL_APPLYING,
    OTA_PULL_FAILED,
} ota_pull_state_t;

typedef struct {
    ota_pull_state_t state;
    ota_manifest_t manifest;
    uint32_t received, total;
    char error[48];
} ota_pull_status_t;

// Boot: reconcile what the last boot left behind (a download waiting, or one just installed).
void ota_pull_init(void);
// Wi-Fi got an address / the clock arrived from NTP: maybe time for the once-a-day check.
void ota_pull_on_ip(void);
void ota_pull_on_time(void);
// An update was found and is not the running build yet (launcher badge and Settings ask
// this); copies the offered version if asked for.
bool ota_pull_pending(char *version, size_t len);
// A check, download or install is in flight. Push-OTA refuses to run alongside one, and back.
bool ota_pull_active(void);
void ota_pull_get_status(ota_pull_status_t *out);
// Manual operations; they refuse (return false / error) when busy or offline. The daily
// check runs through the same code after its own "is it due" arithmetic.
bool ota_pull_start_check(void);
bool ota_pull_start_download(void);
void ota_pull_cancel(void);
// Plugged in (USB or charging). Downloads run on battery; installing - switching the boot
// slot and restarting - happens only on power, and is asked again right before the switch.
bool ota_power_ok(void);
// Switch the boot slot and restart. ESP_ERR_INVALID_STATE without power or a verified image.
esp_err_t ota_pull_apply(void);

// ---------------------------------------------------------------- game catalog (the store app)
// The catalog is static files behind the same host as the firmware OTA data
// (docs/reference/CATALOG.md): /otadb/games/index-watch.json and, per entry,
// /otadb/games/<channel>/<id>.tat. Same plumbing as the pull-OTA client: a short-lived
// worker task per fetch, status polled by the UI, SHA-256 checked while the package
// streams in. One fetch at a time across both kinds.
#define STORE_INDEX_MAX 32768   // the index budget: anything bigger fails before parsing

typedef enum {
    STORE_IDLE = 0,
    STORE_FETCHING,
    STORE_OK,        // index: body ready; package: written and renamed into place
    STORE_FAILED,
} store_state_t;

typedef struct {
    store_state_t state;
    uint32_t received, total;
    char error[48];
} store_status_t;

// Fetch the compact index. On STORE_OK the body is in the component's PSRAM buffer,
// NUL-terminated, valid until the next fetch.
bool store_index_fetch(void);
void store_index_status(store_status_t *out);
const char *store_index_body(void);

// Download one package to dest_path (a .tat path under the games folder): writes
// dest_path.part while streaming, checks sha256_hex (64 hex chars) and the size, then
// renames into place - a half-written file is never visible to the loader's scan.
bool store_package_fetch(const char *url, const char *dest_path, const char *sha256_hex, uint32_t size);
void store_package_status(store_status_t *out);

// Stop the running fetch. The index body becomes invalid; a package fetch removes its
// .part file.
void store_fetch_cancel(void);
bool store_fetch_active(void);

// ---------------------------------------------------------------- update mode
// Set a flag and reboot; the next boot runs the update screen instead of the launcher.
void net_request_update_mode(void);
// Read and clear the flag.
bool net_take_update_mode(void);

#ifdef __cplusplus
}
#endif
