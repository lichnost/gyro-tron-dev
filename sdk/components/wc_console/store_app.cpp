#include "console/store_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "board/board.h"
#include "console/console.h"
#include "console/ui.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "engine/polar.h"
#include "net/net.h"
#include "nvs.h"
#include "storage/storage.h"
#include "tat/tat_api.h"

namespace console {

using wc::Engine;
using wc::Gfx;

static constexpr int C = Gfx::CX;

// The last good index stays on the drive, so the store opens with something to show
// even where there is no network. Same atomic write as everything else that lands
// next to the games.
static constexpr const char *CACHE_PATH = STORAGE_ROOT "/store-index.json";

// ---------------------------------------------------------------- the community prompt

// Installing an unsigned community game asks once per game; the answer is remembered.
// The id is a legal NVS key by construction (the parser only lets [a-z0-9_] through).
static bool communityConfirmed(const char *id)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("store", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, id, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void setCommunityConfirmed(const char *id)
{
    nvs_handle_t h;
    if (nvs_open("store", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, id, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------- lifecycle

void StoreApp::begin(Engine &e)
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

void StoreApp::enter(Engine &e)
{
    // The catalog exists only while the store is open: one PSRAM buffer for the whole
    // parse, gone again on exit.
    catalog_ = static_cast<StoreCatalog *>(heap_caps_malloc(sizeof(StoreCatalog), MALLOC_CAP_SPIRAM));
    refreshInstalled();

    char ssid[33];
    if (net_saved_ssid(ssid, sizeof(ssid))) ssid_ = ssid;
    stale_ = true;
    if (net_state() == NET_CONNECTED) {
        store_index_fetch();
        go(St::Fetch);
    } else if (net_saved_ssid(ssid, sizeof(ssid))) {
        go(St::JoinSaved);
    } else {
        setup_.start();
        go(St::Setup);
    }
}

void StoreApp::leave(Engine &e)
{
    if (store_fetch_active()) store_fetch_cancel();
    if (catalog_) {
        catalog_->~StoreCatalog();
        heap_caps_free(catalog_);
        catalog_ = nullptr;
    }
}

void StoreApp::go(St s)
{
    st_ = s;
    dirty_ = true;
    shown_ = false;
    drawn_pct_ = -1;
    full_ = true;
    done_t_ = 0;
}

void StoreApp::exit(Engine &e)
{
    if (back_) e.switchTo(*back_);
    else e.goHome();
}

void StoreApp::fail(const char *msg)
{
    snprintf(error_, sizeof(error_), "%s", msg);
    go(St::Failed);
}

// ---------------------------------------------------------------- data plumbing

void StoreApp::refreshInstalled()
{
    installed_n_ = loader_scan(installed_, LOADER_MAX_GAMES);
}

const StoreEntry *StoreApp::picked()
{
    if (picked_ < 0 || picked_ >= catalog_->count) return nullptr;
    return &catalog_->entries[picked_];
}

bool StoreApp::runnable(const StoreEntry &e) const
{
    return e.api_major == TAT_API_MAJOR && e.api_minor <= TAT_API_MINOR;
}

int StoreApp::installedVersion(const StoreEntry &e) const
{
    for (int i = 0; i < installed_n_; i++)
        if (strcmp(installed_[i].id, e.id) == 0) return installed_[i].version;
    return 0;
}

bool StoreApp::installed(const StoreEntry &e) const { return installedVersion(e) > 0; }

// One list row. The value column is the state the user acts on: what is installed,
// what the channel is, what refuses to run.
void StoreApp::drawRow(Gfx &g, int row, int y)
{
    if (stale_ && row == 0) {
        ui::rowAt(g, y, "REFRESH", "TAP", ui::ACCENT);
        return;
    }
    const int idx = row - (stale_ ? 1 : 0);
    if (idx < 0 || idx >= catalog_->count) return;
    const StoreEntry &en = catalog_->entries[idx];
    if (en.revoked) {
        ui::rowAt(g, y, en.name, "TAKEN DOWN", ui::DANGER);
        return;
    }
    const int ver = installedVersion(en);
    if (ver > 0) {
        if (ver < en.version) ui::rowAt(g, y, en.name, "UPDATE", ui::GO);
        else ui::rowAt(g, y, en.name, "INSTALLED", ui::GO);
        return;
    }
    if (!runnable(en)) {
        ui::rowAt(g, y, en.name, "OLD FIRMWARE", ui::DANGER);
        return;
    }
    if (en.official) ui::rowAt(g, y, en.name, "OFFICIAL", ui::ACCENT);
    else ui::rowAt(g, y, en.name, "COMMUNITY", ui::DIM);
}

bool StoreApp::loadCache()
{
    FILE *f = fopen(CACHE_PATH, "rb");
    if (!f) return false;
    char *buf = static_cast<char *>(malloc(STORE_INDEX_MAX + 1));
    if (!buf) {
        fclose(f);
        return false;
    }
    const size_t n = fread(buf, 1, STORE_INDEX_MAX, f);
    fclose(f);
    buf[n] = 0;
    const bool ok = store_index_parse(buf, *catalog_);
    free(buf);
    return ok;
}

void StoreApp::saveCache(const char *body)
{
    char part[64];
    snprintf(part, sizeof(part), "%s.part", CACHE_PATH);
    FILE *f = fopen(part, "wb");
    if (!f) return;
    const size_t n = strlen(body);
    if (fwrite(body, 1, n, f) == n) {
        fclose(f);
        remove(CACHE_PATH);
        rename(part, CACHE_PATH);
    } else {
        fclose(f);
        remove(part);
    }
}

// ---------------------------------------------------------------- update

void StoreApp::update(Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);

    switch (st_) {
    case St::JoinSaved:
        if (!shown_) break;   // let "JOINING" reach the screen before blocking
        if (net_join_saved(15000)) {
            store_index_fetch();
            go(St::Fetch);
        } else {
            setup_.start();
            go(St::Setup);
        }
        break;

    case St::Setup:
        switch (setup_.update(e, ges_, dt)) {
        case WifiSetup::Result::Connected:
            store_index_fetch();
            go(St::Fetch);
            break;
        case WifiSetup::Result::Cancelled: exit(e); break;
        default: break;
        }
        break;

    case St::Fetch: {
        store_status_t st;
        store_index_status(&st);
        if (st.state == STORE_OK) {
            if (store_index_parse(store_index_body(), *catalog_)) {
                saveCache(store_index_body());
                stale_ = false;
                list_.reset();
                list_.setCount(catalog_->count);
                go(St::List);
            } else {
                fail("the catalog is damaged");
            }
        } else if (st.state == STORE_FAILED) {
            // No network answer is not the end: the cached list keeps the store usable.
            if (loadCache()) {
                stale_ = true;
                list_.reset();
                list_.setCount(catalog_->count + 1);   // + the REFRESH row
                go(St::List);
            } else {
                fail(st.error[0] ? st.error : "could not fetch the catalog");
            }
        }
        break;
    }

    case St::List: {
        list_.setCount(catalog_->count + (stale_ ? 1 : 0));
        const int row = list_.update(in.touch, ges_, dt);
        if (row >= 0) {
            if (stale_ && row == 0) {
                store_index_fetch();
                go(St::Fetch);
            } else {
                picked_ = row - (stale_ ? 1 : 0);
                go(St::Detail);
            }
            break;
        }
        if (ges_.swipe_right && !list_.dragging()) exit(e);
        break;
    }

    case St::Detail: {
        const StoreEntry *en = picked();
        if (!en) return go(St::List);
        const bool have = installed(*en);
        if (ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y)) {
            if (have) {
                ask_ = Ask::Delete;
                go(St::Confirm);
            } else if (en->revoked) {
                break;   // taken down: the row says so, the button does nothing
            } else if (!runnable(*en)) {
                break;   // the button is drawn dimmed; nothing to do
            } else if (!en->official && !communityConfirmed(en->id)) {
                ask_ = Ask::Community;
                go(St::Confirm);
            } else {
                startInstall(*en);
            }
        } else if ((ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y)) || ges_.swipe_right) {
            go(St::List);
        }
        break;
    }

    case St::Confirm:
        if (ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y)) {
            const StoreEntry *en = picked();
            if (!en) return go(St::List);
            if (ask_ == Ask::Community) {
                setCommunityConfirmed(en->id);
                startInstall(*en);
            } else {
                eraseInstalled(*en);
            }
        } else if ((ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y)) || ges_.swipe_right) {
            go(St::Detail);
        }
        break;

    case St::Downloading: {
        store_status_t st;
        store_package_status(&st);
        if (st.state == STORE_OK) {
            storage_changed();   // the launcher rescans the games folder on its own generation check
            refreshInstalled();
            go(St::Done);
        } else if (st.state == STORE_FAILED) {
            fail(st.error[0] ? st.error : "download failed");
        } else if (ges_.tap && ui::buttonRect(0, 1).hit(ges_.x, ges_.y)) {
            store_fetch_cancel();
        }
        break;
    }

    case St::Done:
        done_t_ += dt;
        if (done_t_ > 1.2f) go(St::List);
        break;

    case St::Failed:
        if (ges_.tap) go(St::List);
        break;
    }
}

void StoreApp::startInstall(const StoreEntry &e)
{
    // Slot and storage checks come before the first byte moves.
    const bool updating = installed(e);
    if (!updating && installed_n_ >= LOADER_MAX_GAMES) return fail("no game slots free - delete one first");
    uint32_t total = 0, free_bytes = 0;
    if (!storage_free_bytes(&total, &free_bytes) || free_bytes < e.size + 64 * 1024) return fail("not enough storage");

    char path[96];
    snprintf(path, sizeof(path), "%s/%s.tat", loader_games_dir(), e.id);
    if (!store_package_fetch(e.url, path, e.sha, e.size)) return fail("could not start the download");
    go(St::Downloading);
}

void StoreApp::eraseInstalled(const StoreEntry &e)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.tat", loader_games_dir(), e.id);
    remove(path);
    storage_changed();
    refreshInstalled();
    list_.invalidate();
    go(St::List);
}

// ---------------------------------------------------------------- drawing

// The changelog-style wrap from the update screen: a scale-2 line fits 36 characters
// across the round panel; break at spaces.
static int wrapText(const char *s, char out[][40], int max_lines)
{
    int n = 0;
    while (*s && n < max_lines) {
        int take = (int)strlen(s);
        if (take > 36) {
            int cut = 36;
            while (cut > 0 && s[cut] != ' ') cut--;
            take = cut > 0 ? cut : 36;
        }
        snprintf(out[n], 40, "%.*s", take, s);
        s += take;
        while (*s == ' ') s++;
        n++;
    }
    return n;
}

static void formatSize(char *out, size_t len, uint32_t bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, len, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes % (1024u * 1024u)) * 10u / (1024u * 1024u)));
    else
        snprintf(out, len, "%u KB", (unsigned)(bytes / 1024));
}

void StoreApp::drawProgress(Gfx &g, bool full, const char *label, uint32_t received, uint32_t total)
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

void StoreApp::draw(Engine &e, Gfx &g)
{
    if (st_ == St::Setup) {
        setup_.draw(g);
        return;
    }
    if (st_ == St::Fetch) {
        if (dirty_) {
            dirty_ = false;
            ui::menuBackground(g);
            g.textCentered(C, C - 30, "STORE", ui::ACCENT, 4, true);
            g.textCentered(C, C + 24, "FETCHING", ui::TEXT, 3, true);
        }
        return;
    }
    if (st_ == St::Downloading) {
        store_status_t st;
        store_package_status(&st);
        drawProgress(g, dirty_, "DOWNLOADING", st.received, st.total);
        if (dirty_) ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL");
        dirty_ = false;
        return;
    }
    if (st_ == St::List) {
        if (full_) {
            ui::menuBackground(g);
            ui::title(g, stale_ ? "STORE (SAVED)" : "STORE", ui::TITLE_Y, stale_ ? ui::DIM : ui::TEXT);
            ui::outlineButton(g, ui::buttonRect(0, 1), "BACK");
            list_.invalidate();
            full_ = false;
        }
        list_.draw(g, [this](Gfx &gg, int row, int y) { drawRow(gg, row, y); });
        shown_ = true;
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
        g.textCentered(C, 150, "GAME STORE", ui::ACCENT, 3, true);
        g.textCentered(C, C, "JOINING", ui::TEXT, 4, true);
        g.textCentered(C, C + 44, ssid_.substr(0, 20).c_str(), ui::DIM, 2, true);
        break;

    case St::Detail: {
        const StoreEntry *en = picked();
        if (!en) return go(St::List);
        const bool have = installed(*en);
        g.textCentered(C, 84, en->name, ui::TEXT, en->name[12] ? 3 : 4, true);
        char line[48];
        snprintf(line, sizeof(line), "by %s", en->author);
        g.textCentered(C, 128, line, ui::DIM, 2, true);
        char size[16];
        formatSize(size, sizeof(size), en->size);
        snprintf(line, sizeof(line), "v%u  %s", en->version, size);
        g.textCentered(C, 154, line, ui::VALUE, 2, true);
        // The channel is the trust story; say it in the badge colour, not just in words.
        g.textCentered(C, 182, en->official ? "OFFICIAL" : "COMMUNITY", en->official ? ui::GO : ui::ACCENT, 2, true);
        char lines[3][40];
        const int n = wrapText(en->summary, lines, 3);
        for (int i = 0; i < n; i++) g.textCentered(C, 214 + i * 26, lines[i], ui::TEXT, 2, true);
        if (have)
            ui::button(g, ui::buttonRect(0, 2), "DELETE", ui::DANGER, wc::colors::white);
        else if (en->revoked)
            ui::outlineButton(g, ui::buttonRect(0, 2), "TAKEN DOWN", ui::DANGER);
        else if (runnable(*en))
            ui::button(g, ui::buttonRect(0, 2), "INSTALL");
        else
            ui::outlineButton(g, ui::buttonRect(0, 2), "NEEDS NEW FW", ui::DIM);
        ui::outlineButton(g, ui::buttonRect(1, 2), "BACK");
        break;
    }

    case St::Confirm: {
        const StoreEntry *en = picked();
        if (!en) return go(St::List);
        if (ask_ == Ask::Community) {
            g.textCentered(C, 110, "COMMUNITY GAME", ui::ACCENT, 3, true);
            g.textCentered(C, 168, "NOT VERIFIED BY", ui::TEXT, 2, true);
            g.textCentered(C, 194, "THE CONSOLE MAKERS", ui::TEXT, 2, true);
            g.textCentered(C, 232, en->name, ui::VALUE, 3, true);
            ui::button(g, ui::buttonRect(0, 2), "INSTALL");
            ui::outlineButton(g, ui::buttonRect(1, 2), "BACK");
        } else {
            g.textCentered(C, 110, "DELETE", ui::DANGER, 4, true);
            g.textCentered(C, 170, en->name, ui::TEXT, 3, true);
            g.textCentered(C, 220, "THE GAME AND ITS", ui::DIM, 2, true);
            g.textCentered(C, 246, "DATA GO AWAY", ui::DIM, 2, true);
            ui::button(g, ui::buttonRect(0, 2), "DELETE", ui::DANGER, wc::colors::white);
            ui::outlineButton(g, ui::buttonRect(1, 2), "BACK");
        }
        break;
    }

    case St::Done:
        g.textCentered(C, C - 20, "INSTALLED!", ui::GO, 5, true);
        break;

    case St::Failed:
        g.textCentered(C, C - 40, "STORE", ui::DANGER, 3, true);
        g.textCentered(C, C, error_, ui::TEXT, 2, true);
        g.textCentered(C, C + 50, "TAP TO CONTINUE", ui::DIM, 2, true);
        break;

    default: break;
    }
}

}  // namespace console
