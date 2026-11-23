// The power menu: SLEEP, SHUT DOWN, RESTART.
//
// Hold PWR anywhere on the console. Every power action is asked for by name, and each says
// what it is doing, and how to come back from it, before the screen goes dark - a dark
// screen alone tells nothing about which of sleep, shutdown or auto-poweroff is running.
//
// The hold is long on purpose - PWR is a game's button too - and so it is not silent: the
// engine shows this screen half way through (Engine::POWER_PEEK_S), with a ring that fills
// for the rest of it. Letting go early goes back to whatever was running.
#pragma once

#include "console/ui.h"
#include "engine/engine.h"
#include "engine/gestures.h"

namespace console {

class PowerMenu : public wc::Game {
public:
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    void redraw() override { dirty_ = true; }

private:
    enum Phase { HOLDING, MENU, GOODBYE };
    enum Choice { SLEEP, SHUT_DOWN, RESTART, CHOICES };

    void choose(wc::Engine &e, Choice c);

    wc::Gestures ges_;
    Phase phase_ = MENU;
    Choice choice_ = SLEEP;
    float t_ = 0;            // seconds in this phase
    bool fresh_ = false;     // the engine blanks the input of the frame it switches on
    bool busy_ = false;      // asked to turn off while a file was being copied
    bool dirty_ = true;
    int ring_lit_ = -1;      // dots of the hold ring already lit on screen; -1 = none drawn
};

}  // namespace console
