// Home screen: a carousel of apps. Swipe (or click PWR) to browse, tap to launch,
// double-click PWR to sleep. BOOT in any app returns here.
#pragma once

#include <vector>

#include "console/console.h"
#include "console/theme.h"
#include "engine/gestures.h"

namespace console {

class Launcher : public wc::Game {
public:
    Launcher(const App *apps, int count) : apps_(apps), n_(count) {}
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;

    // Games are installed and removed while the watch is running. When storage changes,
    // the launcher asks whoever owns the app table to look again; the answer is the new
    // number of apps, or -1 for "nothing is different". The table itself stays where it is.
    using RescanFn = int (*)();
    void setRescan(RescanFn fn) { rescan_ = fn; }

    // The one loaded package, if home needs the room back. A package stays loaded after
    // the player goes home so that returning finds the game as it was left - but a big
    // one holds memory home cannot repaint without (see enter()).
    using EvictGameFn = void (*)();
    void setEvictGame(EvictGameFn fn) { evict_game_ = fn; }

    // Discretionary memory to give back on arriving home - the remote debugger's frame
    // caches (about 870 KB, needed only while someone is watching over USB or Wi-Fi).
    // Home is the one screen that must always be able to repaint, so nothing optional
    // outlives the return to it. The caches re-allocate lazily on the next capture, so
    // screenshotting the home screen itself still works.
    using ReleaseCachesFn = void (*)();
    void setReleaseCaches(ReleaseCachesFn fn) { release_caches_ = fn; }

    // Give the decoded carousel icons (about a third of a megabyte) to a launching
    // game. They are only visible here, and buildIcons() remakes them on the way back.
    void releaseIcons();


    // True while the icons are out to lunch (releaseIcons without a rebuild since).
    bool iconsReleased() const { return builtin_icons_.empty(); }

private:
    static constexpr int ICON_R = 105;
    static constexpr int SPACING = 290;   // neighbors peek in from the edges
    static constexpr int ICON_CY = wc::Gfx::CY - 28;

    void move(int dir);
    void drawIcons(wc::Gfx &g, int shift);
    const Image &iconFor(int app) const;
    void refreshVisible();
    void buildIcons();
    void rescan(wc::Engine &e);
    int count() const { return int(vis_.size()); }
    const App &app(int slot) const { return apps_[vis_[slot]]; }

    const App *apps_;
    int n_;
    int sel_ = 0;                        // a slot in vis_
    int last_app_ = 0;
    std::vector<int> vis_;               // indexes of the apps not hidden in Settings > GAMES
    std::vector<Image> builtin_icons_;   // drawn in code; a theme's icons/<id>.png wins
    uint32_t theme_gen_ = 0;
    RescanFn rescan_ = nullptr;
    EvictGameFn evict_game_ = nullptr;
    ReleaseCachesFn release_caches_ = nullptr;
    uint32_t storage_gen_ = 0;
    wc::Gestures ges_;

    float anim_ = 0;   // horizontal offset of the carousel, eases to 0
    bool full_ = true;
    float hint_t_ = 0;
    float battery_t_ = 9;           // seconds since the battery was read
    int battery_pct_ = -1;
    bool battery_charging_ = false;
    int hint_ = 0;
    bool hint_dirty_ = true;
    int drawn_shift_ = 1 << 30;
};

}  // namespace console
