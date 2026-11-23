// An installed game, as the carousel sees it.
//
// The package is not opened until the player taps its icon. That is deliberate: a package
// comes from a stranger, and an earlier version that loaded everything during start-up
// turned one bad file into three crashed boots and safe mode. Loading on the tap means a
// package that cannot be loaded costs a message on screen, and nothing else.
#pragma once

#include "engine/engine.h"
#include "loader/loader.h"
#include "tat/tat_host.h"

namespace tat {

class PackagedGame : public wc::Game {
public:
    explicit PackagedGame(const loader_entry_t &entry) : entry_(entry) {}
    ~PackagedGame() override;

    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void leave(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    void redraw() override;
    bool keepAwake() const override;
    size_t saveState(void *buf, size_t max) const override;
    bool loadState(const void *buf, size_t len) override;

    const loader_entry_t &entry() const { return entry_; }
    loader_game_t *package() const { return pkg_; }   // for loader_resident()'s asset streaming

private:
    friend void evict_resident();   // needs unload()
    bool load();
    void unload();
    bool ready(wc::Engine &e);

    loader_entry_t entry_;
    loader_game_t *pkg_ = nullptr;
    HostedGame *hosted_ = nullptr;
    bool begun_hosted_ = false;       // the game has had its begin() since it was last loaded
    const char *failure_ = nullptr;   // what to say when it would not load
    bool drawn_failure_ = false;
    // A deep-sleep restore hands the state over before the package is even read; it is
    // applied right after the game's begin(), when there is a game to apply it to.
    uint8_t *pending_state_ = nullptr;
    size_t pending_state_len_ = 0;
};

// Lets go of the one loaded package, if any. The launcher calls this when it is back
// home and cannot afford the repaint next to what the resident game still holds.
void evict_resident();

}  // namespace tat
