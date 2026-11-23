#include "console/theme.h"

#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "console/console.h"
#include "console/ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lodepng.h"
#include "nvs.h"
#include "storage/storage.h"

namespace console {

using wc::Color;

static const char *TAG = "theme";

void Image::release()
{
    heap_caps_free(px);
    heap_caps_free(alpha);
    px = nullptr;
    alpha = nullptr;
    w = h = 0;
}

void drawImage(wc::Gfx &g, const Image &img, int x, int y)
{
    if (!img.valid()) return;
    if (img.alpha) g.blitAlpha(img.px, img.alpha, img.w, img.h, x, y);
    else g.blit(img.px, img.w, img.h, x, y);
}

// "Marble Maze.png", "marble-maze.png" and "MARBLEMAZE.PNG" should all mean the
// same thing: lower-case letters and digits only.
static std::string slug(const std::string &s)
{
    std::string out;
    for (char ch : s)
        if (std::isalnum(static_cast<unsigned char>(ch))) out += char(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// Width and height from the PNG header alone (33 bytes), without decoding. When asked,
// also says whether the format itself rules out translucency (see loadPngBytes).
static bool pngSize(const std::string &path, unsigned &w, unsigned &h, bool *opaque = nullptr, bool warn = false)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char head[33];
    const size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (n < sizeof(head)) return false;

    LodePNGState state;
    lodepng_state_init(&state);
    const unsigned err = lodepng_inspect(&w, &h, &state, head, n);
    if (opaque)
        *opaque = err == 0 && (state.info_png.color.colortype == 0 || state.info_png.color.colortype == 2) &&
                  !state.info_png.color.key_defined;
    lodepng_state_cleanup(&state);
    if (err && warn) ESP_LOGW(TAG, "%s: not a PNG (%s)", path.c_str(), lodepng_error_text(err));
    return err == 0;
}

// Decoding needs the raw scratch and the output copies at once, so check the PNG header
// before touching a huge image someone dropped on the drive.
static bool pngFits(const std::string &path, int max_w, int max_h)
{
    unsigned w = 0, h = 0;
    bool opaque = false;
    if (!pngSize(path, w, h, &opaque, true)) return false;
    // Bigger pictures are shrunk to fit (see loadPng), but decoding still needs
    // the full raw image, so there's a limit on what we'll take on.
    constexpr uint32_t kMaxPixels = 1600000;   // ~1264x1264
    if (w == 0 || h == 0 || uint32_t(w) * h > kMaxPixels) {
        ESP_LOGW(TAG, "%s is %ux%u - too big to load", path.c_str(), w, h);
        return false;
    }
    // Raw scratch (3 B/px opaque, 4 with alpha) + our RGB565 + alpha, with room to spare.
    // The estimate must match what loadPngBytes actually asks for: assuming the
    // 4+2+1 worst case for every picture refuses an opaque background that would
    // have fit - and home sits without one while the memory it needs is there.
    const size_t bpp = opaque ? 5 : 7;
    const size_t needed = size_t(w) * h * bpp + 65536;
    const size_t have = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (needed > have) {
        ESP_LOGW(TAG, "%s needs %u KB, only %u KB free - skipped", path.c_str(), unsigned(needed / 1024),
                 unsigned(have / 1024));
        return false;
    }
    return true;
}

// Decode PNG bytes (from a file read into memory, or embedded in the firmware).
static bool loadPngBytes(const uint8_t *png, size_t len, const char *what, Image &out, int max_w, int max_h)
{
    // The PNG header declares whether the image can be translucent at all: an alpha
    // channel (color type 4 or 6), a palette (which tRNS may have given alpha), or a
    // tRNS colour key. Everything else is opaque by construction, and for those the
    // decode is 24-bit and the alpha plane is never allocated. That plane is 434 KB on
    // a full-screen background, and the decode's peak - scratch plus output - is what
    // decides whether home can repaint next to what the last game left behind.
    LodePNGState probe;
    lodepng_state_init(&probe);
    unsigned iw = 0, ih = 0;
    const unsigned probe_err = lodepng_inspect(&iw, &ih, &probe, png, len);
    const unsigned ct = probe.info_png.color.colortype;
    const bool keyed = probe.info_png.color.key_defined != 0;
    lodepng_state_cleanup(&probe);
    if (probe_err) {
        ESP_LOGW(TAG, "%s: %s", what, lodepng_error_text(probe_err));
        return false;
    }
    const bool opaque = (ct == 0 || ct == 2) && !keyed;

    unsigned char *src = nullptr;
    unsigned w = 0, h = 0;
    const unsigned bpp = opaque ? 3 : 4;
    const unsigned err = opaque ? lodepng_decode24(&src, &w, &h, png, len)
                                : lodepng_decode32(&src, &w, &h, png, len);
    if (err) {
        ESP_LOGW(TAG, "%s: %s", what, lodepng_error_text(err));
        free(src);
        return false;
    }

    // Too big for the screen? Shrink it by a whole-number factor rather than
    // refusing it, so any art someone drops in works.
    unsigned step = 1;
    while ((w + step - 1) / step > unsigned(max_w) || (h + step - 1) / step > unsigned(max_h)) step++;
    const unsigned out_w = (w + step - 1) / step, out_h = (h + step - 1) / step;
    if (step > 1) ESP_LOGI(TAG, "%s: %ux%u shrunk to %ux%u", what, w, h, out_w, out_h);

    out.release();
    out.w = out_w;
    out.h = out_h;
    out.px = static_cast<Color *>(heap_caps_malloc(out_w * out_h * sizeof(Color), MALLOC_CAP_SPIRAM));
    if (!opaque) out.alpha = static_cast<uint8_t *>(heap_caps_malloc(out_w * out_h, MALLOC_CAP_SPIRAM));
    if (!out.px || (!opaque && !out.alpha)) {
        out.release();
        free(src);
        return false;
    }
    bool translucent = false;
    if (step == 1) {
        for (unsigned i = 0; i < w * h; i++) {
            const unsigned char *p = src + i * bpp;
            out.px[i] = wc::rgb(p[0], p[1], p[2]);
            if (out.alpha) {
                out.alpha[i] = p[3];
                translucent |= p[3] != 255;
            }
        }
    } else {
        // Shrink by averaging step x step blocks.
        for (unsigned y = 0; y < out_h; y++) {
            for (unsigned x = 0; x < out_w; x++) {
                unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
                for (unsigned sy = y * step; sy < (y + 1) * step && sy < h; sy++) {
                    for (unsigned sx = x * step; sx < (x + 1) * step && sx < w; sx++) {
                        const unsigned char *p = src + (sy * w + sx) * bpp;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        if (out.alpha) a += p[3];   // p[3] exists only in the 4-byte rows
                        n++;
                    }
                }
                const unsigned i = y * out_w + x;
                out.px[i] = wc::rgb(r / n, g / n, b / n);
                if (out.alpha) {
                    out.alpha[i] = uint8_t(a / n);
                    translucent |= out.alpha[i] != 255;
                }
            }
        }
    }
    free(src);
    if (out.alpha && !translucent) {   // opaque pixels draw with a straight copy
        heap_caps_free(out.alpha);
        out.alpha = nullptr;
    }
    return true;
}

static bool loadPng(const std::string &path, Image &out, int max_w, int max_h)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!pngFits(path, max_w, max_h)) return false;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM));
    if (!buf) return false;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        // A silent failure here once left home black with no trace in the log at all.
        ESP_LOGW(TAG, "%s: could not be opened (%u KB internal free)", path.c_str(),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        heap_caps_free(buf);
        return false;
    }
    const size_t n = std::fread(buf, 1, st.st_size, f);
    std::fclose(f);
    const bool ok = n == size_t(st.st_size) && loadPngBytes(buf, n, path.c_str(), out, max_w, max_h);
    heap_caps_free(buf);
    return ok;
}

bool Theme::builtinIcon(const std::string &app_id, Image &out)
{
    const uint8_t *png = nullptr;
    size_t len = 0;
    if (!storage_builtin_icon(app_id.c_str(), &png, &len)) return false;
    return loadPngBytes(png, len, app_id.c_str(), out, 232, 232);
}

bool Theme::iconFromPng(const uint8_t *png, size_t len, const char *what, Image &out)
{
    return png && len && loadPngBytes(png, len, what, out, 232, 232);
}

static bool parseColor(cJSON *colors, const char *key, Color &out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(colors, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const char *s = item->valuestring;
    if (*s == '#') s++;
    if (std::strlen(s) != 6) return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 16);
    if (*end) return false;
    out = wc::rgb((v >> 16) & 255, (v >> 8) & 255, v & 255);
    return true;
}

Theme &Theme::get()
{
    static Theme t;
    return t;
}

const Image *Theme::icon(const std::string &app_id, const std::string &title) const
{
    auto it = icons_.find(app_id);
    if (it != icons_.end()) return &it->second;
    // Named either way round: "jump.png" for the id, or "Sky Jump.png" for the title.
    //
    // Nothing looser than that. This used to accept a theme file whose name merely
    // appeared anywhere inside the id or title, which meant STARFALL matched star.png and
    // wore SLEEPY STAR's face on the home screen - and the two games looked like one game
    // that had replaced the other. Any id beginning with a four-letter icon name could
    // take it, which is a poor thing to leave lying around when games arrive from
    // strangers. An icon that does not match exactly falls back to the built-in one, which
    // is always right if plain.
    const std::string id = slug(app_id), t = slug(title);
    for (const auto &kv : icons_) {
        const std::string &k = kv.first;
        if (k == id || k == t) return &kv.second;
    }
    return nullptr;
}

void Theme::clear()
{
    background_.release();
    for (auto &kv : icons_) kv.second.release();
    icons_.clear();
    // Built-in palette.
    ui::BG = wc::colors::black;
    ui::TEXT = wc::colors::white;
    ui::LABEL = wc::rgb(225, 225, 225);
    ui::DIM = wc::rgb(150, 150, 150);
    ui::PANEL = wc::rgb(16, 18, 26);
    ui::BOX = wc::rgb(90, 90, 100);
    ui::SCRIM = wc::rgb(30, 32, 40);
    ui::VALUE = wc::colors::cyan;
    ui::ACCENT = wc::colors::yellow;
    ui::GO = wc::rgb(40, 200, 110);
    ui::DANGER = wc::colors::red;
}

// While a theme loads there's nothing on screen to explain the pause, so draw a
// simple progress screen: this runs on the app task, between file loads.
static void drawLoading(wc::Engine *e, const std::string &theme, const std::string &file, int done, int total)
{
    if (!e) return;
    wc::Gfx &g = e->gfx();
    const int cx = wc::Gfx::CX;
    g.clear(wc::colors::black);
    g.textCentered(cx, 170, "LOADING THEME", wc::colors::white, 3, true);
    std::string name = theme;
    for (auto &ch : name) ch = char(std::toupper(ch));
    g.textCentered(cx, 206, name.c_str(), ui::ACCENT, 2, true);

    const int bar_w = 300, bar_x = cx - bar_w / 2, bar_y = 250;
    g.rect(bar_x, bar_y, bar_w, 20, ui::BOX);
    if (total > 0) g.fillRect(bar_x + 2, bar_y + 2, (bar_w - 4) * done / total, 16, ui::GO);
    g.textCentered(cx, 296, file.c_str(), ui::DIM, 2, true);

    e->presenter().present(g);
    e->presenter().flush();
}

bool Theme::load(const std::string &name, wc::Engine *progress_ui)
{
    const int64_t t0 = esp_timer_get_time();
    clear();
    name_ = name;
    generation_++;
    if (!storage_ready()) return false;

    const std::string dir = std::string(STORAGE_THEMES) + "/" + name;
    crumb(("theme " + name).c_str());

    if (FILE *f = std::fopen((dir + "/theme.json").c_str(), "rb")) {
        std::string text;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        if (cJSON *root = cJSON_Parse(text.c_str())) {
            if (cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "colors")) {
                parseColor(c, "background", ui::BG);
                parseColor(c, "text", ui::TEXT);
                parseColor(c, "label", ui::LABEL);
                parseColor(c, "dim", ui::DIM);
                parseColor(c, "panel", ui::PANEL);
                parseColor(c, "box", ui::BOX);
                parseColor(c, "scrim", ui::SCRIM);
                parseColor(c, "value", ui::VALUE);
                parseColor(c, "accent", ui::ACCENT);
                parseColor(c, "go", ui::GO);
                parseColor(c, "danger", ui::DANGER);
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "%s/theme.json isn't valid JSON", name.c_str());
        }
    }

    crumb("background.png");
    drawLoading(progress_ui, name, "theme.json", 0, 3);
    // List the icons first so the progress bar knows how much work there is.
    // The documented place is icons/, but PNGs dropped next to theme.json count
    // too, as long as they aren't the background.
    std::vector<std::string> icon_files;   // paths relative to the theme folder
    for (const std::string &sub : {std::string("icons/"), std::string("")}) {
        if (DIR *d = opendir((dir + "/" + sub).c_str())) {
            while (dirent *e = readdir(d)) {
                std::string file = e->d_name;
                // Skip the junk Windows/macOS leave behind (._name.png, .DS_Store, ...).
                if (file.size() < 5 || file[0] == '.') continue;
                std::string ext = file.substr(file.size() - 4);
                for (auto &ch : ext) ch = char(std::tolower(ch));
                if (ext == ".png") icon_files.push_back(sub + file);
            }
            closedir(d);
        }
    }

    // background.png is the documented name, but any PNG dropped in the theme
    // folder works too (people export art with all sorts of names): take the one
    // called something like "background", else the biggest picture in the folder.
    std::string bg_file;
    {
        uint32_t best_px = 0;
        for (const std::string &file : icon_files) {
            if (file.rfind("icons/", 0) == 0) continue;
            const std::string s = slug(file.substr(0, file.size() - 4));
            unsigned w = 0, h = 0;
            pngSize(dir + "/" + file, w, h);
            const bool named = s.find("background") != std::string::npos || s == "bg" || s.find("back") == 0;
            const uint32_t px = uint32_t(w) * h + (named ? 1u << 30 : 0);
            if (px > best_px) {
                best_px = px;
                bg_file = file;
            }
        }
        if (!bg_file.empty())
            icon_files.erase(std::find(icon_files.begin(), icon_files.end(), bg_file));
        else
            bg_file = "background.png";
    }
    // Whether the theme folder actually offers a background: a decode that failed for
    // lack of memory must be distinguishable from a theme that simply has no art, or the
    // launcher would either stay black forever or retry a background that never existed.
    bool bg_expected = false;
    {
        struct stat st;
        bg_expected = stat((dir + "/" + bg_file).c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }

    const int total = 1 + int(icon_files.size());
    int done = 0;
    drawLoading(progress_ui, name, bg_file.c_str(), done, total);
    loadPng(dir + "/" + bg_file, background_, wc::Gfx::W, wc::Gfx::H);
    ++done;
    std::string icon_names;
    for (const std::string &file : icon_files) {
        const size_t slash = file.rfind('/');
        const std::string base = file.substr(slash == std::string::npos ? 0 : slash + 1);
        const std::string id = slug(base.substr(0, base.size() - 4));
        Image img;
        crumb(base.c_str());
        drawLoading(progress_ui, name, base.c_str(), ++done, total);
        if (loadPng(dir + "/" + file, img, 232, 232)) {
            icons_[id] = img;
            icon_names += (icon_names.empty() ? "" : ", ") + file;
        }
    }
    drawLoading(progress_ui, name, "", total, total);
    const bool ok = !bg_expected || background_.valid();
    if (!ok)
        ESP_LOGW(TAG, "'%s': background.png did not load (%u KB PSRAM free) - the reload will be retried",
                 name.c_str(), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, "loaded '%s' (bg %s: %s; %u icons: %s) in %lld ms", name.c_str(),
             background_.valid() ? "yes" : "no", bg_file.c_str(), unsigned(icons_.size()), icon_names.c_str(),
             (esp_timer_get_time() - t0) / 1000);
    crumb("");
    return ok;
}

std::vector<std::string> Theme::available() const
{
    std::vector<std::string> names;
    if (!storage_ready()) return names;
    int damaged = 0;   // directory slots still 0xFF (erased flash, never written)
    if (DIR *d = opendir(STORAGE_THEMES)) {
        while (dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.' || name == "System Volume Information") continue;
            if (name.compare(0, 6, "FOUND.") == 0) continue;   // chkdsk leftovers
            // An unwritten (0xFF) directory slot comes back as garbage bytes; never offer
            // that (storage_real_entry, the drive-wide rule).
            if (!storage_real_entry(name.c_str())) {
                damaged++;
                continue;
            }
            if (e->d_type != DT_DIR) {
                // Not every filesystem fills in d_type; ask the filesystem directly.
                struct stat st;
                if (stat((std::string(STORAGE_THEMES) + "/" + name).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                    continue;
            }
            names.push_back(name);
        }
        closedir(d);
        // Unwritten (0xFF) slots in a directory cluster are normal on erased flash
        // after Windows adds entries; they're just noise to skip.
        if (damaged) ESP_LOGD(TAG, "%d unwritten slots in %s", damaged, STORAGE_THEMES);
    } else {
        ESP_LOGW(TAG, "no %s folder on the drive", STORAGE_THEMES);
    }
    // Default first, the rest alphabetical.
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        if (a == "Default") return true;
        if (b == "Default") return false;
        return a < b;
    });
    return names;
}

std::string Theme::savedName() const
{
    std::string name = "Default";
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        char buf[64];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "theme", buf, &len) == ESP_OK) name = buf;
        nvs_close(h);
    }
    return name;
}

bool Theme::tryLoad(const std::string &name, wc::Engine *progress_ui, bool at_once, bool fall_back)
{
    const int64_t now = esp_timer_get_time();
    const int64_t wait = 2000000LL << (retry_failures_ > 4 ? 4 : retry_failures_);
    if (!at_once && now - last_retry_us_ < wait) return false;
    last_retry_us_ = now;
    std::string want = name;
    if (fall_back) {
        // A theme folder that was deleted or renamed on the computer falls back to Default.
        const auto names = available();
        if (std::find(names.begin(), names.end(), want) == names.end()) want = "Default";
    }
    if (load(want, progress_ui)) {
        evicted_ = false;
        retry_failures_ = 0;
        storage_gen_ = storage_generation();
        return true;
    }
    retry_failures_++;
    evicted_ = true;   // the images are missing and still wanted; retried at the same pace
    return false;
}

bool Theme::loadActive() { return tryLoad(savedName(), nullptr, false, true); }

void Theme::setActive(const std::string &name)
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "theme", name.c_str());
        nvs_commit(h);
        nvs_close(h);
    }
    // The player just asked for this one, so the load is not paced out - but a failure
    // is recorded like any other: Settings' poll() and home retry it with the backoff.
    tryLoad(name, nullptr, true, false);
}

namespace ui {

void clearScreen(wc::Gfx &g, int dx, int dy)
{
    const Image &bg = Theme::get().background();
    if (bg.valid() && bg.w == wc::Gfx::W && bg.h == wc::Gfx::H && !bg.alpha && dx == 0 && dy == 0) {
        g.blit(bg.px, bg.w, bg.h, 0, 0);
        return;
    }
    g.clear(BG);   // fills whatever the shifted picture leaves uncovered
    drawImage(g, bg, (wc::Gfx::W - bg.w) / 2 + dx, (wc::Gfx::H - bg.h) / 2 + dy);
}

void restoreBg(wc::Gfx &g, int x, int y, int w, int h, int dx, int dy)
{
    const Image &bg = Theme::get().background();
    g.setClip(x, y, w, h);
    g.fillRect(x, y, w, h, BG);
    drawImage(g, bg, (wc::Gfx::W - bg.w) / 2 + dx, (wc::Gfx::H - bg.h) / 2 + dy);
    g.clearClip();
}

}  // namespace ui

bool Theme::needsReload() const { return storage_generation() != storage_gen_ && storage_ready(); }

bool Theme::poll()
{
    if (!needsReload()) return false;
    return loadActive();
}

void Theme::releaseForGame()
{
    if (evicted_ && !background_.px && icons_.empty()) return;
    background_.release();
    for (auto &kv : icons_) kv.second.release();
    icons_.clear();
    evicted_ = true;
    generation_++;
}

void Theme::reloadWithProgress(wc::Engine &e)
{
    // Reload when the drive changed or the images were let go, through the same paced
    // tryLoad as everywhere: a failure leaves the load pending (evicted_, storage_gen_
    // untouched) and the next home frame retries it after the backoff, instead of a
    // failed reload becoming final - the home screen sat black until a reboot for
    // exactly that reason once. The pace also keeps a theme that genuinely has no
    // background from turning every frame into a directory listing, or flashing the
    // loading screen twice a second while memory is short.
    if (!needsReload() && !evicted_) return;
    tryLoad(name_, &e, false, true);
}

}  // namespace console
