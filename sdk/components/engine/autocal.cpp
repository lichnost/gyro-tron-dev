#include "engine/autocal.h"

#include <algorithm>
#include <cmath>

#include "engine/calib_fit.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace wc::cal {

namespace {

const char *TAG = "autocal";

void (*s_save)(const TiltCal &) = nullptr;

constexpr float STILL_DPS = 2.0f;    // corrected gyro below this, sustained, is "rest"
// Windows may open up to this corrected rate, but only the strict ones feed coverage.
// The reason is the lockout the strict gate alone had: after the manual flow measured
// the bias, self-heating and weather can walk the true bias further than STILL_DPS away
// from the stored one - then w never dips under 2, no window ever opens, the EMA never
// learns, and the stale bias sits in the delivery forever. A rate in 2..6 at rest IS the
// bias reading; a real turn is excluded not by the rate gate but by the accel direction
// check below (a 5 deg/s turn breaks the JUMP_G window within a second, so it never
// completes).
constexpr float RESIDUAL_DPS = 6.0f;
constexpr float STILL_G_LO = 0.9f, STILL_G_HI = 1.1f;
constexpr float JUMP_G = 0.03f;      // a jolt inside the window restarts it
constexpr float BIAS_EMA = 0.1f;     // per window; a nightstand converges in minutes
constexpr float QUALITY_FLOOR_DEG = 0.5f;   // a refit is never accepted past this

}  // namespace

Autocal &autocal()
{
    static Autocal a;
    return a;
}

void Autocal::setSave(void (*save)(const TiltCal &)) { s_save = save; }

void Autocal::pushBias()
{
    // The EMA lives in board frame; the record stores screen frame, so only the bias goes
    // through the map - offset and gain pass by untouched.
    TiltCal cal = Input::calibration();
    const AxMap m = cal.has_map ? unpackAxMap(cal.axmap) : guessAxMap();
    for (int i = 0; i < 3; i++) cal.bias[i] = m.sgn[i] * bias_[m.idx[i]];
    Input::setCalibration(cal);
}

void Autocal::feed(const Tilt &raw)
{
    if (reset_req_.exchange(false)) {
        n_ = 0;
        bins_ = new_bins_ = 0;
        for (int b = 0; b < BINS; b++) have_[b] = false;
        // Re-seed the EMA from what the manual flow just measured, so it does not drag
        // the fresh record back toward the old bias.
        const TiltCal cal = Input::calibration();
        const AxMap m = cal.has_map ? unpackAxMap(cal.axmap) : guessAxMap();
        for (int i = 0; i < 3; i++) bias_[m.idx[i]] = m.sgn[i] * cal.bias[i];
        bias_seen_ = true;
    }

    const TiltCal cal = Input::calibration();
    const AxMap m = cal.has_map ? unpackAxMap(cal.axmap) : guessAxMap();
    const float a_in[3] = {raw.ax, raw.ay, raw.az}, g_in[3] = {raw.gx, raw.gy, raw.gz};
    float a[3], g[3];
    applyAxMap(a_in, m, a);
    applyAxMap(g_in, m, g);
    const float w = std::sqrt((g[0] - cal.bias[0]) * (g[0] - cal.bias[0]) + (g[1] - cal.bias[1]) * (g[1] - cal.bias[1]) +
                              (g[2] - cal.bias[2]) * (g[2] - cal.bias[2]));
    const float am = std::sqrt(a_in[0] * a_in[0] + a_in[1] * a_in[1] + a_in[2] * a_in[2]);
    const bool quiet = w < RESIDUAL_DPS && am > STILL_G_LO && am < STILL_G_HI;
    if (!quiet || (n_ >= 5 && (std::fabs(a_in[0] - asum_[0] / n_) > JUMP_G || std::fabs(a_in[1] - asum_[1] / n_) > JUMP_G ||
                               std::fabs(a_in[2] - asum_[2] / n_) > JUMP_G))) {
        n_ = 0;
        w_exc_ = 0;
        for (int i = 0; i < 3; i++) asum_[i] = gsum_[i] = 0;
        return;
    }
    if (w >= STILL_DPS) w_exc_++;   // the window may finish loose; only a strict one feeds bins
    for (int i = 0; i < 3; i++) asum_[i] += a_in[i], gsum_[i] += g_in[i];
    if (++n_ < WINDOW_N) return;

    float mean_a[3], mean_g[3];
    for (int i = 0; i < 3; i++) mean_a[i] = asum_[i] / n_, mean_g[i] = gsum_[i] / n_;
    n_ = 0;
    w_exc_ = 0;
    for (int i = 0; i < 3; i++) asum_[i] = gsum_[i] = 0;

    // Bias first, every window - it is the day-to-day win and it never wears anything.
    if (!bias_seen_) {
        for (int i = 0; i < 3; i++) bias_[i] = mean_g[i];
        bias_seen_ = true;
    } else {
        for (int i = 0; i < 3; i++) bias_[i] += BIAS_EMA * (mean_g[i] - bias_[i]);
    }
    pushBias();
    if (w_exc_) return;   // a loose window learned the bias; it earns no coverage point

    // Then coverage: one newest point per gravity direction, so a night on the same
    // nightstand is one point, not eleven thousand.
    const float un = 1.0f / std::sqrt(mean_a[0] * mean_a[0] + mean_a[1] * mean_a[1] + mean_a[2] * mean_a[2]);
    const float uz = mean_a[2] * un;
    int sector = int((std::atan2(mean_a[1], mean_a[0]) + float(M_PI)) / (float(M_PI) / 6.0f));
    if (sector > 11) sector = 11;
    int band = int((uz + 1.0f) / (2.0f / 6.0f));
    if (band > 5) band = 5;
    if (band < 0) band = 0;
    const int bin = sector * 6 + band;
    if (!have_[bin]) {
        have_[bin] = true;
        bins_++;
        new_bins_++;
    }
    for (int i = 0; i < 3; i++) pts_[bin][i] = mean_a[i];

    considerRefit(esp_timer_get_time());
}

void Autocal::considerRefit(int64_t now)
{
    if (bins_ < MIN_BINS || new_bins_ < NEW_BINS) return;
    if (now - last_refit_us_ < REFIT_EVERY_US) return;
    last_refit_us_ = now;

    float board_pts[BINS][3];
    int kept = 0;
    for (int b = 0; b < BINS; b++)
        if (have_[b]) {
            for (int i = 0; i < 3; i++) board_pts[kept][i] = pts_[b][i];
            kept++;
        }
    wc::cal::SphereCal sc;
    float worst = 0;
    if (!fitSphere(board_pts, kept, sc, worst)) {
        ESP_LOGI(TAG, "refit rejected: %d bins do not identify the model", kept);
        bins_ = new_bins_ = 0;
        for (int b = 0; b < BINS; b++) have_[b] = false;
        return;
    }
    // A refit must not quietly make a good model worse: the worst misfit it shows has to
    // stay within 20% of the stored one (half a degree, whichever is larger), or the
    // whole accumulator is dropped and the refresher waits for better coverage.
    const TiltCal cal = Input::calibration();
    const float stored = cal.residual_deg > 0 ? cal.residual_deg : QUALITY_FLOOR_DEG;
    const float limit = std::max(1.2f * stored, QUALITY_FLOOR_DEG);
    if (worst > limit) {
        ESP_LOGI(TAG, "refit rejected: worst %.2f deg over the %.2f deg limit", worst, limit);
        bins_ = new_bins_ = 0;
        for (int b = 0; b < BINS; b++) have_[b] = false;
        return;
    }
    TiltCal next = cal;   // map flags and provenance carry; only the model moves
    const AxMap m = cal.has_map ? unpackAxMap(cal.axmap) : guessAxMap();
    storeSphereCal(sc, bias_, m, next, false);
    next.residual_deg = worst;
    next.auto_refits++;
    ESP_LOGI(TAG, "refit ok: worst %.2f deg, %d bins, refit #%u", worst, kept, next.auto_refits);
    Input::setCalibration(next);
    if (s_save) s_save(next);
    new_bins_ = 0;
}

void Autocal::reset() { reset_req_ = true; }

}  // namespace wc::cal
