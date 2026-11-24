// The game store: browse the catalog and install games over Wi-Fi, straight on the
// watch (docs/plans/GAME_DISTRIBUTION_PLAN.md §6.2, the catalog in
// docs/reference/CATALOG.md). A built-in console app, not a package: installing means
// writing into /data/Games, which the loader sandbox rightly forbids a .tat to do.
//
// The catalog lives in RAM only while the store is open - one PSRAM parse buffer, one
// fixed-size entry array, both freed on exit. The last good index is cached on the
// drive, so a store opened offline shows the cached list with a stale marker and a
// refresh row instead of an error.
#pragma once

#include <string>
#include <vector>

#include "console/scroll_list.h"
#include "console/store_index.h"
#include "console/wifi_setup.h"
#include "engine/engine.h"
#include "engine/gestures.h"
#include "loader/loader.h"

namespace console {

class StoreApp : public wc::Game {
public:
    explicit StoreApp(wc::Game *back = nullptr) : back_(back) {}
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void leave(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override
    {
        return st_ == St::JoinSaved || st_ == St::Fetch || st_ == St::Downloading || st_ == St::Done;
    }

private:
    enum class St { JoinSaved, Setup, Fetch, List, Detail, Confirm, Downloading, Done, Failed };
    enum class Ask { None, Community, Delete };   // what the Confirm screen is confirming
    void go(St s);
    void exit(wc::Engine &e);
    void refreshInstalled();
    const StoreEntry *picked();
    bool runnable(const StoreEntry &e) const;
    bool installed(const StoreEntry &e) const;
    void startInstall(const StoreEntry &e);
    void eraseInstalled(const StoreEntry &e);
    void drawRow(wc::Gfx &g, int row, int y);
    int installedVersion(const StoreEntry &e) const;   // 0 when not installed
    void drawProgress(wc::Gfx &g, bool full, const char *label, uint32_t received, uint32_t total);
    void fail(const char *msg);
    bool loadCache();
    void saveCache(const char *body);

    wc::Game *back_;
    St st_ = St::Fetch;
    Ask ask_ = Ask::None;
    bool dirty_ = true;
    bool shown_ = false;
    wc::Gestures ges_;
    WifiSetup setup_;
    std::string ssid_;

    StoreCatalog *catalog_ = nullptr;   // PSRAM; exists only while the store is open
    bool stale_ = true;                 // the list came from the cache, not a fresh fetch

    loader_entry_t installed_[LOADER_MAX_GAMES];
    int installed_n_ = 0;
    int picked_ = -1;                   // catalog index behind the Detail screen

    ui::ScrollList list_;
    bool full_ = true;
    char error_[48] = {0};

    // Progress ring (the update screen's pattern): rim pixels and their angle, drawn
    // incrementally so a percentage change touches only its own arc.
    std::vector<uint32_t> ring_px_;
    std::vector<uint16_t> ring_ang_;
    uint16_t ring_drawn_ = 0;
    int drawn_pct_ = -1;
    float done_t_ = 0;
};

}  // namespace console
