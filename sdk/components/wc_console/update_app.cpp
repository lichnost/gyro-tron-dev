#include "console/update_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "board/board.h"
#include "console/console.h"
#include "console/ui.h"
#include "engine/polar.h"
#include "esp_system.h"
#include "storage/storage.h"

namespace console {

using wc::Engine;
using wc::Gfx;

static constexpr int C = Gfx::CX;

void UpdateApp::begin(Engine &e)
{
    wc::Polar::init();
    for (int i = 0; i < Gfx::W * Gfx::H; i++) {
        const uint16_t r16 = wc::Polar::radius16(i);
        if (r16 >= 219 * 16 && r16 <= 231 * 16) {
            ring_px_.push_back(i);
            ring_ang_.push_back(uint16_t(wc::Polar::angle(i) + 16384));   // 0 at 12 o'clock
        }
    }
}

void UpdateApp::enter(Engine &e)
{
    char ssid[33];
    const bool saved = net_saved_ssid(ssid, sizeof(ssid));
    if (saved) ssid_ = ssid;

    if (net_state() == NET_CONNECTED) {
        ota_server_start();
        go(St::Ready);
    } else if (saved) {
        go(St::JoinSaved);
    } else {
        setup_.start();
        go(St::Setup);
    }
}

void UpdateApp::go(St s)
{
    st_ = s;
    dirty_ = true;
    shown_ = false;
    drawn_pct_ = -1;
}

void UpdateApp::exit(Engine &e)
{
    if (back_) e.switchTo(*back_);
    else esp_restart();   // update mode: reboot into the console
}

// A pull step went wrong: the push screen's Failed state speaks for both flows.
void UpdateApp::fail(const char *msg)
{
    snprintf(ota_.error, sizeof(ota_.error), "%s", msg);
    go(St::Failed);
}

void UpdateApp::update(Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);
    const bool busy = st_ == St::Flashing || st_ == St::Done || st_ == St::Downloading || st_ == St::Applying;

    // BOOT is "back to menu" everywhere. The engine handles it when there's a home
    // app; update mode has none, so leave by rebooting into the console. Not while
    // the watch is mid-download or mid-install: those end on their own.
    if (!back_ && (in.pressed & wc::BTN_A) && !busy) esp_restart();

    switch (st_) {
    case St::JoinSaved:
        if (!shown_) break;   // let "JOINING" reach the screen before blocking
        if (net_join_saved(15000)) {
            ota_server_start();
            go(St::Ready);
        } else {
            setup_.start();
            go(St::Setup);
        }
        break;

    case St::Setup:
        switch (setup_.update(e, ges_, dt)) {
        case WifiSetup::Result::Connected:
            ssid_ = setup_.ssid();
            ota_server_start();
            go(St::Ready);
            break;
        case WifiSetup::Result::Cancelled: exit(e); break;
        default: break;
        }
        break;

    case St::Ready: {
        ota_get_status(&ota_);
        if (ota_.state == OTA_RECEIVING) {
            go(St::Flashing);
            break;
        }
        ota_pull_get_status(&pull_);
        if (pull_.state != seen_pull_) {   // the row's value changed under us
            seen_pull_ = pull_.state;
            dirty_ = true;
        }
        if (pull_.state == OTA_PULL_APPLYING) {   // an install started from somewhere else
            go(St::Applying);
            break;
        }
        if (ges_.tap && ui::rowRect(0).hit(ges_.x, ges_.y)) {
            switch (pull_.state) {
            case OTA_PULL_AVAILABLE: go(St::Available); break;
            case OTA_PULL_DOWNLOADING: go(St::Downloading); break;
            case OTA_PULL_READY: go(St::ReadyInstall); break;
            case OTA_PULL_CHECKING: break;   // already on it; the row shows "..."
            default: ota_pull_start_check(); break;   // Idle, UpToDate or Failed: ask again
            }
        } else if (ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y)) {
            setup_.start();
            go(St::Setup);
        } else if ((ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y)) || ges_.swipe_right) {
            exit(e);
        }
        break;
    }

    case St::Flashing:
        ota_get_status(&ota_);
        if (ota_.state == OTA_DONE) go(St::Done);
        if (ota_.state == OTA_FAILED) go(St::Failed);
        break;

    case St::Done:
        done_t_ += dt;
        if (done_t_ > 1.5f) esp_restart();
        break;

    case St::Failed:
        if (ges_.tap) go(St::Ready);
        break;

    case St::Available:
        ota_pull_get_status(&pull_);
        if (pull_.state == OTA_PULL_DOWNLOADING) {   // started from somewhere else
            go(St::Downloading);
            break;
        }
        if (ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y)) {
            if (ota_pull_start_download()) go(St::Downloading);
            else fail(pull_.error[0] ? pull_.error : "could not start the download");
        } else if ((ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y)) || ges_.swipe_right) {
            go(St::Ready);
        }
        break;

    case St::Downloading:
        ota_pull_get_status(&pull_);
        if (pull_.state == OTA_PULL_READY) go(St::ReadyInstall);
        else if (pull_.state == OTA_PULL_AVAILABLE) go(St::Available);   // cancelled
        else if (pull_.state == OTA_PULL_FAILED) fail(pull_.error[0] ? pull_.error : "download failed");
        else if (ges_.tap && ui::buttonRect(0, 1).hit(ges_.x, ges_.y)) ota_pull_cancel();
        break;

    case St::ReadyInstall: {
        ota_pull_get_status(&pull_);
        if (pull_.state == OTA_PULL_FAILED) {   // the install refused: say so
            fail(pull_.error[0] ? pull_.error : "install failed");
            break;
        }
        if (pull_.state == OTA_PULL_APPLYING) {
            go(St::Applying);
            break;
        }
        // The charger is a pair of I2C reads, so it is polled once a second, not per frame.
        power_t_ += dt;
        if (power_t_ > 1.0f) {
            power_t_ = 0;
            const bool ok = ota_power_ok();
            if (ok != power_ok_) {
                power_ok_ = ok;
                dirty_ = true;   // the INSTALL button and the prompt change with it
            }
        }
        if (ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y)) {
            if (ota_pull_apply() == ESP_OK) go(St::Applying);
            else {
                power_ok_ = false;   // refused: no power, or the image was pulled out from under us
                dirty_ = true;
            }
        } else if ((ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y)) || ges_.swipe_right) {
            go(St::Ready);
        }
        break;
    }

    case St::Applying:
        break;   // the worker switches the boot slot and restarts the watch
    }
}

// ------------------------------------------------------------------ drawing

// The pull value on the Ready screen's UPDATE row - the whole pull flow hangs off it.
static void pullRowValue(const ota_pull_status_t &pull, char *out, size_t len, wc::Color *color)
{
    using ui::ACCENT, ui::DANGER, ui::DIM, ui::GO;
    switch (pull.state) {
    case OTA_PULL_CHECKING:
    case OTA_PULL_APPLYING:
        snprintf(out, len, "...");
        *color = ACCENT;
        break;
    case OTA_PULL_UPTODATE:
        snprintf(out, len, "LATEST");
        *color = DIM;
        break;
    case OTA_PULL_AVAILABLE:
        snprintf(out, len, "NEW %s", pull.manifest.version);
        *color = GO;
        break;
    case OTA_PULL_DOWNLOADING:
        snprintf(out, len, "...");
        *color = ACCENT;
        break;
    case OTA_PULL_READY:
        snprintf(out, len, "INSTALL");
        *color = GO;
        break;
    case OTA_PULL_FAILED:
        snprintf(out, len, "ERROR");
        *color = DANGER;
        break;
    default:
        snprintf(out, len, "CHECK");
        *color = ACCENT;
        break;
    }
}

void UpdateApp::drawReady(Gfx &g)
{
    g.textCentered(C, 88, safeMode() ? "SAFE MODE" : "WI-FI UPDATE", safeMode() ? ui::DANGER : ui::ACCENT, 3, true);
    char value[40];
    wc::Color color;
    pullRowValue(pull_, value, sizeof(value), &color);
    ui::row(g, 0, "UPDATE", value, color);
    g.textCentered(C, 196, NET_HOSTNAME ".local", ui::VALUE, 2, true);
    g.textCentered(C, 224, net_ip(), ui::TEXT, 2, true);
    std::string s = ssid_.size() > 20 ? ssid_.substr(0, 19) + "~" : ssid_;
    g.textCentered(C, 252, s.c_str(), ui::DIM, 2, true);
    g.textCentered(C, 300, "KEY", ui::DIM, 2, true);
    g.textCentered(C, 326, net_device_key(), ui::ACCENT, 2, true);
    ui::outlineButton(g, ui::buttonRect(0, 2), "WI-FI");
    ui::outlineButton(g, ui::buttonRect(1, 2), "EXIT");
}

static void formatSize(char *out, size_t len, uint32_t bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, len, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes % (1024u * 1024u)) * 10u / (1024u * 1024u)));
    else
        snprintf(out, len, "%u KB", (unsigned)(bytes / 1024));
}

// The changelog is one short line from version.json; break it into screen-sized lines
// at spaces (a scale-2 line fits 36 characters across the round panel).
static int wrapText(const char *s, char out[][40], int max_lines)
{
    int n = 0;
    while (*s && n < max_lines) {
        int take = (int)strlen(s);
        if (take > 36) {
            int cut = 36;
            while (cut > 0 && s[cut] != ' ') cut--;
            take = cut > 0 ? cut : 36;   // one very long word gets a hard break
        }
        snprintf(out[n], 40, "%.*s", take, s);
        s += take;
        while (*s == ' ') s++;
        n++;
    }
    return n;
}

void UpdateApp::drawAvailable(Gfx &g)
{
    g.textCentered(C, 88, "NEW VERSION", ui::ACCENT, 3, true);
    g.textCentered(C, 148, pull_.manifest.version, ui::GO, 5, true);
    char size[24];
    formatSize(size, sizeof(size), pull_.manifest.size);
    g.textCentered(C, 200, size, ui::DIM, 2, true);
    char lines[3][40];
    const int n = wrapText(pull_.manifest.changelog, lines, 3);
    for (int i = 0; i < n; i++) g.textCentered(C, 244 + i * 26, lines[i], ui::TEXT, 2, true);
    ui::button(g, ui::buttonRect(0, 2), "DOWNLOAD");
    ui::outlineButton(g, ui::buttonRect(1, 2), "BACK");
}

void UpdateApp::drawReadyInstall(Gfx &g)
{
    g.textCentered(C, 88, "UPDATE READY", ui::GO, 3, true);
    g.textCentered(C, 152, pull_.manifest.version, ui::TEXT, 4, true);
    char size[24];
    formatSize(size, sizeof(size), pull_.manifest.size);
    g.textCentered(C, 204, size, ui::DIM, 2, true);
    // Downloads are fine on battery; the restart is not. Say which one this watch is in.
    if (power_ok_) {
        g.textCentered(C, 262, "PLUGGED IN", ui::GO, 2, true);
        ui::button(g, ui::buttonRect(0, 2), "INSTALL");
    } else {
        g.textCentered(C, 254, "CONNECT USB OR CHARGER", ui::ACCENT, 2, true);
        g.textCentered(C, 280, "TO INSTALL", ui::ACCENT, 2, true);
        ui::outlineButton(g, ui::buttonRect(0, 2), "INSTALL");
    }
    ui::outlineButton(g, ui::buttonRect(1, 2), "BACK");
}

void UpdateApp::drawProgress(Gfx &g, bool full, const char *label, uint32_t received, uint32_t total)
{
    if (full) {
        ui::menuBackground(g);
        for (uint32_t i : ring_px_) g.pixels()[i] = wc::rgb(40, 44, 54);
        g.markAllDirty();
        g.textCentered(C, C - 40, label, ui::TEXT, 3, true);
        ring_drawn_ = 0;
        drawn_pct_ = -1;
    }
    const int pct = total ? int(uint64_t(received) * 100 / total) : 0;
    if (pct == drawn_pct_) return;

    // Extend the ring from the last drawn angle to the new one; mark only that arc dirty.
    const uint16_t to = uint16_t(std::min<uint32_t>(65535, uint32_t(pct) * 65535 / 100));
    int x0 = Gfx::W, y0 = Gfx::H, x1 = -1, y1 = -1;
    for (size_t k = 0; k < ring_px_.size(); k++) {
        const uint16_t a = ring_ang_[k];
        if (a < ring_drawn_ || a > to) continue;
        const uint32_t i = ring_px_[k];
        g.pixels()[i] = ui::GO;
        const int x = i % Gfx::W, y = i / Gfx::W;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    if (x1 >= 0) g.markDirty(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    ring_drawn_ = to;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    ui::restoreMenuBg(g, C - 90, C, 180, 50);
    g.textCentered(C, C + 24, buf, ui::GO, 6, true);
    drawn_pct_ = pct;
}

void UpdateApp::draw(Engine &e, Gfx &g)
{
    if (st_ == St::Setup) {
        setup_.draw(g);
        return;
    }
    if (st_ == St::Flashing || st_ == St::Downloading) {
        const char *label = st_ == St::Flashing ? "UPDATING" : "DOWNLOADING";
        const uint32_t rx = st_ == St::Flashing ? ota_.received : pull_.received;
        const uint32_t tot = st_ == St::Flashing ? ota_.total : pull_.total;
        drawProgress(g, dirty_, label, rx, tot);
        if (dirty_ && st_ == St::Downloading) ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL");
        dirty_ = false;
        return;
    }
    if (!dirty_) {
        shown_ = true;
        return;
    }
    dirty_ = false;
    ui::menuBackground(g);

    switch (st_) {
    case St::JoinSaved:
        g.textCentered(C, 150, "WI-FI UPDATE", ui::ACCENT, 3, true);
        g.textCentered(C, C, "JOINING", ui::TEXT, 4, true);
        g.textCentered(C, C + 44, ssid_.substr(0, 20).c_str(), ui::DIM, 2, true);
        break;
    case St::Ready: drawReady(g); break;
    case St::Available: drawAvailable(g); break;
    case St::ReadyInstall: drawReadyInstall(g); break;
    case St::Applying:
        g.textCentered(C, C - 20, "INSTALLING", ui::GO, 5, true);
        g.textCentered(C, C + 30, "RESTARTING", ui::DIM, 2, true);
        break;
    case St::Done:
        g.textCentered(C, C - 20, "UPDATED!", ui::GO, 5, true);
        g.textCentered(C, C + 30, "RESTARTING", ui::DIM, 2, true);
        break;
    case St::Failed:
        g.textCentered(C, C - 40, "UPDATE FAILED", ui::DANGER, 3, true);
        g.textCentered(C, C, ota_.error, ui::TEXT, 2, true);
        g.textCentered(C, C + 50, "TAP TO CONTINUE", ui::DIM, 2, true);
        break;
    default: break;
    }
}

}  // namespace console
