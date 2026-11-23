// Wi-Fi firmware update screen: push from a computer, pull from the site (docs/reference/OTA.md).
//
// Two ways in:
//  - From Settings while Wi-Fi is connected: opens directly (`back` = Settings).
//  - Update mode: Settings reboots into it when Wi-Fi is off, so a game session
//    never pays for Wi-Fi it didn't ask for (`back` = nullptr, exit reboots).
//
// The push flow (a computer POSTs a .bin) is the Ready/Flashing/Done/Failed loop it always
// was. The pull flow hangs off the UPDATE row on the Ready screen and walks its own
// screens: Available -> Downloading -> ReadyInstall -> (reboot). Downloading is allowed on
// battery; installing switches the boot slot and is only ever done on USB power.
#pragma once

#include <vector>

#include "console/wifi_setup.h"
#include "engine/engine.h"
#include "engine/gestures.h"
#include "net/net.h"

namespace console {

class UpdateApp : public wc::Game {
public:
    explicit UpdateApp(wc::Game *back = nullptr) : back_(back) {}
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    // Waiting for or installing firmware: don't power off underneath it.
    bool keepAwake() const override
    {
        return st_ == St::Ready || st_ == St::Flashing || st_ == St::Done || st_ == St::Downloading ||
               st_ == St::ReadyInstall || st_ == St::Applying;
    }

private:
    enum class St { JoinSaved, Setup, Ready, Flashing, Done, Failed, Available, Downloading, ReadyInstall, Applying };
    void go(St s);
    void exit(wc::Engine &e);
    void drawReady(wc::Gfx &g);
    void drawAvailable(wc::Gfx &g);
    void drawReadyInstall(wc::Gfx &g);
    void drawProgress(wc::Gfx &g, bool full, const char *label, uint32_t received, uint32_t total);
    void fail(const char *msg);

    wc::Game *back_;
    St st_ = St::JoinSaved;
    bool dirty_ = true;
    bool shown_ = false;
    wc::Gestures ges_;
    WifiSetup setup_;
    std::string ssid_;

    ota_status_t ota_{};           // push transfer, polled from the server
    ota_pull_status_t pull_{};     // pull client, polled every frame while relevant
    ota_pull_state_t seen_pull_ = OTA_PULL_IDLE;   // last pull state the screen was drawn with
    int drawn_pct_ = -1;
    float done_t_ = 0;
    float power_t_ = 1;            // seconds since the charger was last asked
    bool power_ok_ = false;        // cached: each ask is a pair of I2C transactions
    std::vector<uint32_t> ring_px_;   // rim pixels for the progress ring
    std::vector<uint16_t> ring_ang_;  // their angle from 12 o'clock, clockwise
    uint16_t ring_drawn_ = 0;
};

}  // namespace console
