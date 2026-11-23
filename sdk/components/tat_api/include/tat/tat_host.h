// The console's side of the game contract: wraps a tat_game_t so the launcher can treat
// it like any other app. Built-in games are registered at startup; installed ones will
// arrive the same way once the loader lands, which is the point of doing it like this.
#pragma once

#include "engine/engine.h"
#include "tat/tat_api.h"

namespace tat {

// Fills in the api table a game is handed. Safe to call more than once.
const tat_api_t *api();

// Called once when a game's allocation fails and once more before the retry, so the
// console can let go of its own non-essential caches - the remote debugger's frame
// copies, which rebuild on demand. Without it a big game cannot use the memory the
// debugger happened to be holding.
void api_set_evict_hook(void (*fn)(void));

// Runs the hook without a failed allocation behind it. A package's bundle and image both
// want large contiguous PSRAM blocks and load exactly once, so the moment to ask for
// them is with every discretionary cache already out of the way - not after a first ask
// discovered the memory was holding a screenshot and sixteen carousel icons.
void api_release_for_game(void);

class HostedGame : public wc::Game {
public:
    explicit HostedGame(const tat_game_t &game);
    ~HostedGame() override { unload(); }

    // Tell the game it is going, then give back whatever it took and did not return:
    // memory, canvases, sprite sheets. Safe to call twice, and on a game that never began.
    void unload();

    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void leave(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    void redraw() override;
    bool keepAwake() const override;
    size_t saveState(void *buf, size_t max) const override;
    bool loadState(const void *buf, size_t len) override;

    const tat_game_t &desc() const { return g_; }

private:
    HostedGame(const HostedGame &) = delete;
    HostedGame &operator=(const HostedGame &) = delete;

    // A copy, not a reference. The descriptor lives in the game's own memory, where the
    // game can rewrite it: a reference let a game retarget its save namespace by editing
    // its id, and its entry points by editing the function pointers. What the host sees
    // is fixed at load time. The id gets its own buffer as well, because the struct copy
    // only copies the pointer to the string, and the string is still the game's.
    tat_game_t g_;
    char id_[16];
};

}  // namespace tat
