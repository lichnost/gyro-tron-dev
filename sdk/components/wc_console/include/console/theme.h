// Themes live on the Gyro-Tron drive as plain files:
//   /data/Theme/<Name>/theme.json          colors
//   /data/Theme/<Name>/background.png      466x466, behind the launcher and menus
//   /data/Theme/<Name>/icons/<app id>.png  app icons (210x210)
// Images are decoded once at load into RGB565 + alpha. Anything a theme leaves
// out falls back to the built-in look.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "engine/gfx.h"

namespace console {

struct Image {
    int w = 0, h = 0;
    wc::Color *px = nullptr;
    uint8_t *alpha = nullptr;   // nullptr = opaque
    bool valid() const { return px != nullptr; }
    void release();
};

void drawImage(wc::Gfx &g, const Image &img, int x, int y);

class Theme {
public:
    static Theme &get();

    // The saved choice, else Default. False when the load was paced out or failed -
    // a failed load stays pending (see tryLoad) and is retried by poll()/reloadWithProgress().
    bool loadActive();
    void setActive(const std::string &name);            // save + load
    const std::string &name() const { return name_; }
    std::vector<std::string> available() const;         // folder names under Theme/

    // Reload if the drive came back from a computer. Returns true when it reloaded.
    bool poll();
    // True when the drive changed under us and the theme should be re-read.
    bool needsReload() const;
    // Reload, drawing a progress screen (loading a big theme takes a moment).
    void reloadWithProgress(wc::Engine &e);
    uint32_t generation() const { return generation_; }
    // Give the decoded images (about a megabyte of PSRAM) back to a launching game.
    // The palette stays, so the console keeps its colours; the images reload on the
    // next home screen, where a game is not.
    void releaseForGame();

    const Image &background() const { return background_; }
    // The firmware's own icon for an app (the Default theme's PNG), for the
    // carousel when the active theme doesn't provide one.
    static bool builtinIcon(const std::string &app_id, Image &out);
    // An icon from PNG bytes that came from somewhere else - a package brings its own.
    static bool iconFromPng(const uint8_t *png, size_t len, const char *what, Image &out);
    // The icon for an app: icons/<id>.png, or any icon file whose name looks
    // like the app's id or title ("Marble Maze.png", "grand-prix.png", ...).
    const Image *icon(const std::string &app_id, const std::string &title) const;

private:
    // Returns false when nothing was read (storage absent) or the theme folder's own
    // background failed to decode (memory short) - a reload that must be tried again.
    bool load(const std::string &name, wc::Engine *progress_ui = nullptr);
    // The one paced loader behind every reload - the boot-time choice, storage changing
    // under the console, a theme picked in Settings, eviction for a game. A load that
    // fails leaves the images missing and wanted, and the next attempt waits out the
    // backoff (2 s doubling to 32 s), so a failure is never final and never a per-frame
    // storm; storage_gen_ is advanced only by a success, so a failed load stays pending.
    // `at_once` skips the wait (the player just picked this theme); `fall_back` swaps a
    // vanished folder for Default. False when paced out or the load failed.
    bool tryLoad(const std::string &name, wc::Engine *progress_ui, bool at_once, bool fall_back);
    std::string savedName() const;   // the theme choice in NVS, "Default" when unset
    void clear();

    std::string name_ = "Default";
    Image background_;
    std::map<std::string, Image> icons_;
    uint32_t generation_ = 0;
    bool evicted_ = false;   // the decoded images are gone - released for a game, or a load that failed - and wanted again
    int64_t last_retry_us_ = 0;   // a failed reload retries at most every 2 s, not per frame
    int retry_failures_ = 0;      // consecutive failed reloads; paces the retry backoff
    uint32_t storage_gen_ = 0;
};

}  // namespace console
