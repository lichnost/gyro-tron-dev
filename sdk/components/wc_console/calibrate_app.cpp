#include "console/calibrate_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/audio.h"
#include "console/console.h"
#include "console/ui.h"
#include "engine/autocal.h"
#include "esp_log.h"

namespace console {

namespace {

const char *TAG = "calibrate";

constexpr int DISH_X = wc::Gfx::CX, DISH_Y = 206, DISH_R = 72, RING_R = 86;
constexpr int LINE1_Y = 312, LINE2_Y = 338;
constexpr float WINDOW_S = 1.5f;          // how long a table pose has to lie still
constexpr float AIM_S = 1.0f;             // in-hand poses settle faster than tables change
constexpr float STILL_DPS = 2.5f;         // more gyro wobble than this restarts the reading
constexpr float STILL_G = 0.03f;
// In-hand poses (AIM, and the tilt rescue) can't be held as still as a table pose, and
// they don't have to be: the reading is a one-second average, so a slightly wobbly hand
// still yields good data - the gate only decides WHEN the reading is taken. Tuned above
// resting hand tremor (5-10 deg/s): a gate the hand always trips just restarts the
// window forever and the reading never lands.
constexpr float HAND_DPS = 7.0f;
constexpr float HAND_G = 0.08f;
constexpr float FLAT_Z = 0.9f;            // |screen z| above this = lying flat
constexpr float TURN_MIN = 150.0f, TURN_MAX = 210.0f;
// Bearing step between stops, negative on purpose. The ball is a real bubble: it rises to
// the dish's high point, and rolling the watch about the glass normal leaves that point
// where it was in the world - so the ball walks the dish AGAINST the turn, one full turn
// per turn, like a compass needle. Rolling clockwise (the natural wrist roll) walks it
// counter-clockwise, and the marks are laid out to be met in that order.
constexpr float SPIN_MARK = -90.0f;
constexpr float QUALITY_DEG = 0.5f;       // worst misfit the fit may show and still pass
// What counts as a reading worth keeping. These are sanity limits on a measurement that
// has ALREADY been taken while the watch held still (that is what the window above is
// for) - they are not a judgement on how good the sensor is. An early version capped the
// gyro at 15 deg/s, and the watch it was written on has a gyro sitting at 18: every
// calibration failed, and the screen blamed the table. A part can be out by a lot and
// still be perfectly correctable; what these catch is a reading taken while the watch was
// genuinely on the move, or a sensor that has nothing sensible to say at all.
constexpr float MAX_TILT_G = 0.35f;        // about 20 degrees off level
constexpr float MAX_DRIFT_DPS = 60.0f;     // beyond this the gyro is not merely biased
constexpr float MIN_SCALE = 0.85f, MAX_SCALE = 1.15f;   // trust the measured 1 g this far

enum Msg {
    M_INTRO, M_AIM, M_AIM_AGAIN, M_HANDS_OFF, M_FLAT, M_TURN, M_TURN_MORE, M_TURN_BACK,
    M_TILTUP, M_SPIN, M_SPIN_STOP, M_TILT, M_TILT2, M_RESULT, M_FAIL,
};

// Each pose message names the pose and its angle, so the flow reads as a list the user
// can follow: 1 = upright, 2 = flat, 3 = tilted 45 in hand (then spun), 4 = rescue tilt.
// The held poses are worded like aimdiag's legs - concrete, physical anchors (screen at
// your face, top edge away "like opening a laptop") instead of "upright"/"top up",
// which read as ambiguous while the watch is in hand.
const char *const LINES[][2] = {
    {"MEASURES THE SENSOR", "AND THE AXIS MAP - START?"},                    // M_INTRO
    {"POSE 1: SCREEN AT YOUR FACE,", "TILT TOP AWAY, LOOK DOWN AT IT"},      // M_AIM
    {"AGAIN POSE 1: SCREEN AT FACE,", "TILT TOP AWAY, LOOK DOWN AT IT"},     // M_AIM_AGAIN
    {"", "HOLD STILL..."},                                                   // M_HANDS_OFF
    {"POSE 2: ON THE TABLE -", "FLAT, SCREEN UP, HANDS OFF"},                // M_FLAT
    {"FLAT ON THE TABLE - TURN", "IT HALF A TURN, LET GO"},                  // M_TURN
    {"A BIT MORE...", "HALF A TURN, THEN LET GO"},                           // M_TURN_MORE
    {"TOO FAR - TURN IT BACK", "TO THE MARK"},                               // M_TURN_BACK
    {"POSE 3: HOLD LIKE POSE 1,", "THEN ROLL IT LIKE A WHEEL"},              // M_TILTUP
    {"ROLL IT LIKE A WHEEL -", "BALL WALKS AGAINST THE TURN"},               // M_SPIN
    {"ON THE MARK - TAP,", "HOLD STILL"},                                     // M_SPIN_STOP
    {"ONE MORE: TILT 45 DEG LEFT,", "SCREEN AT YOU - HOLD IT"},              // M_TILT
    {"NOW TILT 45 DEG THE OTHER", "WAY - SCREEN AT YOU, HOLD"},              // M_TILT2
    {"", "BUBBLE IN THE MIDDLE?"},                                           // M_RESULT
    {"", ""},                                                                // M_FAIL
};

void ringDots(wc::Gfx &g, float from_deg, float to_deg, wc::Color c)
{
    if (to_deg < from_deg) std::swap(from_deg, to_deg);
    for (float a = from_deg; a <= to_deg; a += 4.0f) {
        const float r = (a - 90.0f) * float(M_PI) / 180.0f;
        g.fillCircle(DISH_X + int(std::lround(std::cos(r) * RING_R)), DISH_Y + int(std::lround(std::sin(r) * RING_R)), 5, c);
    }
}

}  // namespace

void CalibrateApp::enter(wc::Engine &e) { setPhase(INTRO); }

void CalibrateApp::setPhase(Phase p)
{
    phase_ = p;
    full_ = true;
    win_.reset();
    armed_ = false;
    if (p == INTRO || p == AIM) tilt_asked_ = false;
    switch (p) {
    case INTRO: msg_ = M_INTRO; break;
    case AIM: msg_ = map_attempts_ > 0 ? M_AIM_AGAIN : M_AIM; break;
    case FLAT1: msg_ = M_FLAT; break;
    case TURN: msg_ = M_TURN, turned_ = 0; break;
    case TILTUP: msg_ = M_TILTUP; break;
    case SPIN: msg_ = M_SPIN; break;
    case TILT: msg_ = tilt_asked_ ? M_TILT2 : M_TILT; break;
    case RESULT: msg_ = M_RESULT; break;
    case FAIL: msg_ = M_FAIL; break;
    }
}

bool CalibrateApp::sample(float dt, float still_dps, float still_g)
{
    const float a[3] = {raw_.ax, raw_.ay, raw_.az}, gy[3] = {raw_.gx, raw_.gy, raw_.gz};
    // Anything that moves it (a finger lifting off, a bumped table) starts the reading over.
    if (win_.n >= 5) {
        for (int i = 0; i < 3; i++) {
            if (std::fabs(a[i] - win_.a[i] / win_.n) > still_g || std::fabs(gy[i] - win_.g[i] / win_.n) > still_dps) {
                win_.reset();
                break;
            }
        }
    }
    for (int i = 0; i < 3; i++) win_.a[i] += a[i], win_.g[i] += gy[i];
    win_.n++;
    win_.t += dt;
    const float window = phase_ == AIM || phase_ == TILT ? AIM_S : WINDOW_S;
    return win_.t >= window && win_.n >= 30;
}

void CalibrateApp::takeReading(int slot)
{
    const float n = float(win_.n);
    for (int i = 0; i < 3; i++) pts_[slot][i] = win_.a[i] / n, gsum_[slot][i] = win_.g[i] / n;
    npts_ = std::max(npts_, slot + 1);
    ESP_LOGI(TAG, "reading %d: a=(%.3f %.3f %.3f)", slot, pts_[slot][0], pts_[slot][1], pts_[slot][2]);
}

void CalibrateApp::deriveMap()
{
    // deriveAxMap owns the pose checks, including the recline sign that rules out a
    // mirrored mount - see its header comment. Here a rejection just asks again - and
    // keeps the reason, so a third strike can say WHICH pose was off and how.
    wc::cal::AxMap m;
    int why = wc::cal::AXMAP_OK;
    if (wc::cal::deriveAxMap(pts_[0], pts_[1], m, &why)) {
        map_ = m;
        map_ok_ = true;
        map_attempts_ = 0;
        ESP_LOGI(TAG, "map x<-a%u%+d y<-a%u%+d z<-a%u%+d", m.idx[0], int(m.sgn[0]), m.idx[1], int(m.sgn[1]),
                 m.idx[2], int(m.sgn[2]));
        return;
    }
    static const char *const WHY[] = {"shaken", "repeated", "snap", "mirror", "aim tilt", "recline", "flat pose"};
    ESP_LOGI(TAG, "map derive rejected (attempt %d): %s", map_attempts_ + 1,
             WHY[std::min(why - 1, 6)]);
    map_why_ = why;
    map_attempts_++;
}

void CalibrateApp::finishOldModel(bool two_readings)
{
    // The pre-v2 math, kept as the SKIP path: screen-frame x/y offsets from the screen-up
    // reading(s) - the half-turn pair cancels the table's slope, a single one trusts it -
    // the gyro bias from the same windows, and one scalar for gain from the magnitude.
    // Stored as a v2 record in the frame the measured map delivers, so the map this run
    // measured still applies.
    const int last = two_readings ? 2 : 1;
    const float k = two_readings ? 0.5f : 1.0f;
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    for (int r = 1; r <= last; r++) {
        float as[3], gs[3];
        wc::cal::applyAxMap(pts_[r], map_, as);
        wc::cal::applyAxMap(gsum_[r], map_, gs);
        ax += k * as[0], ay += k * as[1], az += k * as[2];
        gx += k * gs[0], gy += k * gs[1], gz += k * gs[2];
    }
    found_ = wc::TiltCal{};
    found_.offset[0] = ax, found_.offset[1] = ay;
    found_.bias[0] = gx, found_.bias[1] = gy, found_.bias[2] = gz;
    // Lying level, the only force on it is gravity, so whatever the sensor reads for the
    // length of that vector is what it thinks 1 g is.
    const float magnitude = std::sqrt(ax * ax + ay * ay + az * az);
    const float scale = magnitude > 0.1f ? 1.0f / magnitude : 1.0f;
    found_.gain[0] = found_.gain[1] = found_.gain[2] = std::clamp(scale, MIN_SCALE, MAX_SCALE);
    found_.has_map = map_ok_;
    found_.axmap = wc::cal::packAxMap(map_);
    ESP_LOGI(TAG, "skip: old model from %d reading(s), 1g reads %.4f (x%.4f)", last, magnitude, found_.gain[0]);

    fail_line1_ = fail_line2_ = nullptr;
    const float tilt = std::hypot(ax, ay);
    const float drift = std::sqrt(gx * gx + gy * gy + gz * gz);
    if (tilt > MAX_TILT_G) {
        fail_line1_ = "IT WASN'T LYING FLAT";
        fail_line2_ = "SCREEN UP ON A LEVEL TABLE";
    } else if (drift > MAX_DRIFT_DPS) {
        fail_line1_ = "IT WAS STILL MOVING";
        fail_line2_ = "HANDS OFF, THEN TRY AGAIN";
    } else if (scale < MIN_SCALE || scale > MAX_SCALE) {
        fail_line1_ = "THE SENSOR LOOKS WRONG";
        fail_line2_ = "GRAVITY DIDN'T READ AS 1 G";
    }
    const bool ok = fail_line1_ == nullptr;
    wc::audio::play(ok ? wc::audio::Tone{.f0 = 660, .f1 = 990, .ms = 140} : wc::audio::Tone{.f0 = 300, .f1 = 180, .ms = 220});
    setPhase(ok ? RESULT : FAIL);
}

bool CalibrateApp::runFit()
{
    float bias_board[3] = {0, 0, 0};
    for (int r = 0; r < npts_; r++)
        for (int i = 0; i < 3; i++) bias_board[i] += gsum_[r][i] / npts_;

    wc::cal::SphereCal sc;
    float worst = 0;
    if (!wc::cal::fitSphere(pts_, npts_, sc, worst)) {
        if (!tilt_asked_) {
            // The points left a parameter combination free - the left tilt missed, or a
            // pose came out degenerate. One more, tilted the other way, breaks the tie;
            // ask for that one instead of a generic failure.
            tilt_asked_ = true;
            ESP_LOGI(TAG, "fit degenerate - asking for the opposite tilt");
            setPhase(TILT);
            return false;
        }
        fail_line1_ = "THE SENSOR LOOKS WRONG";
        fail_line2_ = "THE POINTS DONT MAKE A SPHERE";
        wc::audio::play({.f0 = 300, .f1 = 180, .ms = 220});
        setPhase(FAIL);
        return false;
    }
    ESP_LOGI(TAG, "fit ok, worst %.2f deg over %d points", worst, npts_);
    if (worst > QUALITY_DEG) {
        fail_line1_ = "A READING WAS OFF";
        fail_line2_ = "HOLD STILLER AT THE STOPS - REDO";
        wc::audio::play({.f0 = 300, .f1 = 180, .ms = 220});
        setPhase(FAIL);
        return false;
    }
    found_ = wc::TiltCal{};
    wc::cal::storeSphereCal(sc, bias_board, map_, found_);
    fit_ = sc;
    found_.residual_deg = worst;
    const float drift = std::sqrt(found_.bias[0] * found_.bias[0] + found_.bias[1] * found_.bias[1] +
                                  found_.bias[2] * found_.bias[2]);
    if (drift > MAX_DRIFT_DPS) {
        fail_line1_ = "IT WAS STILL MOVING";
        fail_line2_ = "HANDS OFF, THEN TRY AGAIN";
        wc::audio::play({.f0 = 300, .f1 = 180, .ms = 220});
        setPhase(FAIL);
        return false;
    }
    wc::audio::play({.f0 = 660, .f1 = 990, .ms = 140});
    setPhase(RESULT);
    return true;
}

wc::Tilt CalibrateApp::mapped() const
{
    // applyAxMap cannot alias, so both vectors go through scratch buffers.
    wc::Tilt out;
    const float a_in[3] = {raw_.ax, raw_.ay, raw_.az}, g_in[3] = {raw_.gx, raw_.gy, raw_.gz};
    float a[3], g[3];
    wc::cal::applyAxMap(a_in, map_, a);
    wc::cal::applyAxMap(g_in, map_, g);
    out.ax = a[0], out.ay = a[1], out.az = a[2];
    out.gx = g[0], out.gy = g[1], out.gz = g[2];
    return out;
}

void CalibrateApp::leave(wc::Engine &e)
{
    if (back_) e.switchTo(*back_);
    else e.goHome();
}

void CalibrateApp::update(wc::Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);
    raw_ = wc::Input::rawTilt();   // board frame: this screen measures the sensor itself

    const bool left = ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y);
    const bool right = ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y);
    const bool single = ges_.tap && ui::buttonRect(0, 1).hit(ges_.x, ges_.y);
    const wc::Tilt view = mapped();
    const bool flat = std::fabs(view.az) > FLAT_Z;
    // Every reading waits for a tap: the user gets into the pose at their own pace and
    // taps to start the stillness countdown - an auto-running window used to fill in
    // while they were still turning the watch, and a reading of that pose failed the fit.
    const bool tap_ready = ges_.tap && ges_.y < ui::BUTTON_Y;

    switch (phase_) {
    case INTRO:
        if (left || ges_.swipe_right) return leave(e);
        if (right) return setPhase(AIM);
        if ((in.clicked & wc::BTN_B) && tiltCalibrated()) {
            clearTiltCalibration();
            wc::audio::play({.f0 = 500, .f1 = 300, .ms = 120});
            full_ = true;
        }
        break;

    case AIM:
        if (single) return setPhase(INTRO);
        if (!armed_) {
            if (tap_ready) { armed_ = true, win_.reset(), full_ = true; }
            break;
        }
        if (sample(dt, HAND_DPS, HAND_G)) {
            takeReading(0);
            wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
            setPhase(FLAT1);
        }
        break;

    case FLAT1:
        if (single) return setPhase(INTRO);
        msg_ = M_FLAT;
        if (!armed_) {
            if (tap_ready) { armed_ = true, win_.reset(), full_ = true; }
            break;
        }
        if (sample(dt, STILL_DPS, STILL_G)) {
            takeReading(1);
            deriveMap();
            if (map_attempts_ >= 3) {
                // Say which pose failed and how, from the last rejection's reason.
                switch (map_why_) {
                case wc::cal::AXMAP_REPEATED:
                    fail_line1_ = "POSE 1 WAS TOO FLAT";
                    fail_line2_ = "- TILT THE TOP AWAY, REDO";
                    break;
                case wc::cal::AXMAP_AIM:
                    fail_line1_ = "POSE 1 WAS TOO FLAT";
                    fail_line2_ = "SCREEN AT FACE, TOP AWAY - REDO";
                    break;
                case wc::cal::AXMAP_RECLINE:
                    fail_line1_ = "POSE 1 TILTED THE WRONG";
                    fail_line2_ = "WAY - TOP AWAY, NOT TO YOU";
                    break;
                case wc::cal::AXMAP_MIRROR:
                    fail_line1_ = "POSES LOOK MIRRORED";
                    fail_line2_ = "CHECK POSE 1 TILT - REDO";
                    break;
                case wc::cal::AXMAP_FLAT:
                    fail_line1_ = "POSE 2 WAS NOT FLAT";
                    fail_line2_ = "SCREEN UP ON THE TABLE - REDO";
                    break;
                case wc::cal::AXMAP_SNAP:
                    fail_line1_ = "A POSE WAS SKEWED OR ROLLED";
                    fail_line2_ = "HOLD THEM SQUARE - REDO";
                    break;
                default:
                    fail_line1_ = "A READING CAUGHT MOVEMENT";
                    fail_line2_ = "HANDS OFF, THEN REDO";
                    break;
                }
                wc::audio::play({.f0 = 300, .f1 = 180, .ms = 220});
                return setPhase(FAIL);
            }
            if (!map_ok_) return setPhase(AIM);   // steady first, then flat again
            // The flip mark and the turn baseline come from the stored mean, not the
            // live sample - the reading is what the window averaged, not the last frame.
            float as[3], gs[3];
            wc::cal::applyAxMap(pts_[1], map_, as);
            wc::cal::applyAxMap(gsum_[1], map_, gs);
            az_up_ = as[2];
            first_gz_ = gs[2];
            wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
            setPhase(TURN);
        }
        break;

    case TURN: {
        if (left) return setPhase(INTRO);
        if (right) return finishOldModel(false);   // SKIP: trust the table
        turned_ += (view.gz - first_gz_) * dt;
        const float t = std::fabs(turned_);
        msg_ = t < 20 ? M_TURN : t < TURN_MIN ? M_TURN_MORE : t > TURN_MAX ? M_TURN_BACK : M_TURN;
        if (t >= TURN_MIN && t <= TURN_MAX && flat) {
            // Far enough; take the second reading once the tap confirms it's let go of.
            if (!armed_) {
                if (tap_ready) { armed_ = true, win_.reset(), full_ = true; }
                break;
            }
            if (sample(dt, STILL_DPS, STILL_G)) {
                takeReading(2);
                wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
                setPhase(TILTUP);
            }
            if (win_.t > 0.35f) msg_ = M_HANDS_OFF;
        } else {
            win_.reset();
        }
        break;
    }

    case TILTUP: {
        if (left) return setPhase(INTRO);
        if (right) return finishOldModel(true);   // SKIP: today's whole model, plus the map
        // The tap gates the jump to SPIN too: without it the tilt check could fire while
        // the arm was still lifting the watch into the pose.
        if (!armed_) {
            if (tap_ready) { armed_ = true, win_.reset(), full_ = true; }
            break;
        }
        // Off the table and roughly steady: not flat (the spin needs gravity off its
        // axis) and reading about 1 g (held, not shaken). The exact tilt is the user's
        // choice - the ring spreads whatever it gets.
        const float amag = std::sqrt(view.ax * view.ax + view.ay * view.ay + view.az * view.az);
        if (std::fabs(view.az) < 0.8f && amag > 0.85f && amag < 1.15f) {
            wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
            stops_ = 0, ticked_ = false;
            win_.reset();
            setPhase(SPIN);
        }
        break;
    }

    case SPIN: {
        if (left) return setPhase(INTRO);
        // The bubble IS the dial. Rolling the tilted watch in its own plane (about the
        // glass normal) leaves the dish's high point where it was in the world, so the
        // bubble walks the dish against the turn, one full turn per turn, like a compass
        // needle - an instantaneous accelerometer reading, unlike the gyro integral this
        // replaced, which drifted and wobbled with every hand correction. (A spin about
        // the world vertical would move nothing: gravity lies on that axis. The roll is
        // what works.) A stop is the bubble sitting on the next ring mark; four of them
        // quarter the circle.
        bubble_deg_ = std::atan2(-view.ay, -view.ax) * 57.29578f + 90.0f;
        if (bubble_deg_ < 0) bubble_deg_ += 360.0f;
        float diff = bubble_deg_ - stops_ * SPIN_MARK;
        if (diff > 180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        const float hm = std::hypot(view.ax, view.ay);
        const bool on_mark = hm > 0.35f && std::fabs(diff) < 12.0f;
        if (on_mark && !ticked_) {
            // The mark: two short chirps, unmistakable against every other sound here.
            const wc::audio::Tone chirp{.f0 = 990, .f1 = 990, .ms = 70};
            const wc::audio::Tone again{.f0 = 990, .f1 = 990, .ms = 70, .delay_ms = 110};
            const wc::audio::Tone pair[2] = {chirp, again};
            wc::audio::play(pair, 2);
            ticked_ = true;
        }
        msg_ = ticked_ ? M_SPIN_STOP : M_SPIN;
        // Held in hand at each stop: the same relaxed gates the aim pose uses. Each stop
        // needs a fresh tap - the hand is still moving between stops.
        if (!armed_) {
            if (tap_ready) { armed_ = true, win_.reset(), full_ = true; }
            break;
        }
        if (on_mark && sample(dt, HAND_DPS, HAND_G)) {
            takeReading(3 + stops_);
            stops_++;
            ticked_ = false;
            armed_ = false;   // the next stop needs its own tap
            win_.reset();
            wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
            if (stops_ >= 4) {
                runFit();
                return;
            }
        }
        break;
    }

    case TILT:
        if (single) return setPhase(INTRO);
        msg_ = tilt_asked_ ? M_TILT2 : M_TILT;
        if (!armed_) {
            if (tap_ready) { armed_ = true, win_.reset(), full_ = true; }
            break;
        }
        if (sample(dt, HAND_DPS, HAND_G)) {
            takeReading(tilt_asked_ ? 8 : 7);
            runFit();
            return;
        }
        break;

    case RESULT:
        if (left) return setPhase(FLAT1);   // REDO: the poses, not the aim
        if (right) {
            saveTiltCalibration(found_);
            wc::cal::autocal().reset();
            ESP_LOGI(TAG, "saved v2: residual %.2f deg, map %s", found_.residual_deg,
                     found_.has_map ? "measured" : "guess");
            wc::audio::play({.f0 = 880, .f1 = 1320, .ms = 160});
            return leave(e);
        }
        break;

    case FAIL:
        if (left) return leave(e);
        if (right) return setPhase(AIM);   // REDO from the top: the map may have been the problem
        break;
    }
    if ((in.clicked & wc::BTN_B) && phase_ != INTRO) setPhase(INTRO);

    // One line a second, monitor-visible (tag "calibrate"): what the sensor said in board
    // axes and what a game would see through the correction, side by side. The bring-up
    // answer to "did the sign land": pose 1 must read view=(0, +0.7, -0.7) - down along
    // +y with the recline's negative z - and the bubble must sit at the dish's top.
    bench_t_ += dt;
    if (bench_t_ >= 1.0f) {
        bench_t_ = 0;
        const wc::TiltCal cal = phase_ == RESULT ? found_ : wc::Input::calibration();
        wc::Tilt view_now;
        wc::cal::applyPipeline(cal, raw_, view_now);
        // map= is the measured map's view - the one the bubble and the spin stops use.
        // Pose 1 must read map=(0, +0.7, -0.7); a clockwise roll must walk its xy
        // counter-clockwise. If view= and map= disagree, the saved record is stale.
        const wc::Tilt mv = mapped();
        ESP_LOGI(TAG, "bench raw=(%.3f %.3f %.3f) g=(%.0f %.0f %.0f) view=(%.3f %.3f %.3f) map=(%.3f %.3f %.3f)",
                 raw_.ax, raw_.ay, raw_.az, raw_.gx, raw_.gy, raw_.gz, view_now.ax, view_now.ay, view_now.az,
                 mv.ax, mv.ay, mv.az);
    }
}

void CalibrateApp::draw(wc::Engine &e, wc::Gfx &g)
{
    if (msg_ != drawn_msg_) full_ = true;
    if (full_) {
        ui::menuBackground(g);
        ui::title(g, "CALIBRATE");
        drawn_msg_ = msg_;
        const char *l1 = LINES[msg_][0], *l2 = LINES[msg_][1];
        char buf[48];
        if (phase_ == RESULT) {
            if (found_.residual_deg > 0) {
                snprintf(buf, sizeof(buf), "QUALITY %.1f DEG  MAP %s", found_.residual_deg,
                         found_.has_map ? "OK" : "SKIP");
            } else {
                const float deg = std::asin(std::min(1.0f, std::hypot(found_.offset[0], found_.offset[1]))) * 57.29578f;
                const float drift = std::sqrt(found_.bias[0] * found_.bias[0] + found_.bias[1] * found_.bias[1] +
                                              found_.bias[2] * found_.bias[2]);
                snprintf(buf, sizeof(buf), "TILT %.1f DEG  DRIFT %.0f/S", deg, drift);
            }
            l1 = buf;
        } else if (phase_ == FAIL && fail_line1_) {
            l1 = fail_line1_, l2 = fail_line2_;
        }
        ui::hint(g, LINE1_Y, l1, ui::TEXT);
        ui::hint(g, LINE2_Y, l2, phase_ == FAIL ? ui::DANGER : ui::DIM);
        if (phase_ == INTRO && tiltCalibrated()) ui::hint(g, 96, "SAVED - PWR CLEARS IT", ui::GO);
        // The pose prompt sits above the dish; arming (full redraw) clears it.
        const bool waits_tap = phase_ == AIM || phase_ == FLAT1 || phase_ == TURN || phase_ == TILTUP ||
                                phase_ == SPIN || phase_ == TILT;
        if (waits_tap && !armed_) ui::hint(g, 96, "TAP WHEN IN POSE", ui::GO);
#ifdef CAL_BENCH
        if (phase_ == RESULT && found_.residual_deg > 0) {
            // Per-reading misfit in degrees, board frame: the bring-up answer to "which
            // reading was the sloppy one".
            char row[64];
            int off = 0;
            for (int r = 0; r < npts_; r++) {
                float d[3], m2 = 0;
                for (int i = 0; i < 3; i++)
                    d[i] = pts_[r][i] - fit_.offset[i], m2 += fit_.gain[i] * fit_.gain[i] * d[i] * d[i];
                off += snprintf(row + off, sizeof(row) - off, "%s%.1f", r ? " " : "", std::fabs(std::sqrt(m2) - 1) * 57.29578f);
                if (r == 3 || r == npts_ - 1) {
                    ui::hint(g, r == 3 ? 96 : 122, row, ui::DIM);
                    off = 0;
                }
            }
        }
#endif
        switch (phase_) {
        case INTRO:
            ui::outlineButton(g, ui::buttonRect(0, 2), "BACK");
            ui::button(g, ui::buttonRect(1, 2), "START");
            break;
        case AIM: ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL"); break;
        case FLAT1: ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL"); break;
        case TURN:
            ui::outlineButton(g, ui::buttonRect(0, 2), "CANCEL");
            ui::outlineButton(g, ui::buttonRect(1, 2), "SKIP");
            break;
        case TILTUP:
            ui::outlineButton(g, ui::buttonRect(0, 2), "CANCEL");
            ui::outlineButton(g, ui::buttonRect(1, 2), "SKIP");
            break;
        case SPIN: ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL"); break;
        case TILT: ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL"); break;
        case RESULT:
            ui::outlineButton(g, ui::buttonRect(0, 2), "REDO");
            ui::button(g, ui::buttonRect(1, 2), "SAVE");
            break;
        case FAIL:
            ui::outlineButton(g, ui::buttonRect(0, 2), "BACK");
            ui::button(g, ui::buttonRect(1, 2), "REDO");
            break;
        }
        full_ = false;
    } else {
        const int r = RING_R + 8;
        ui::restoreMenuBg(g, DISH_X - r, DISH_Y - r, r * 2, r * 2);
    }

    // The live part: a bubble level in a dish, with a progress ring around it.
    g.fillCircle(DISH_X, DISH_Y, DISH_R, ui::PANEL);
    g.circle(DISH_X, DISH_Y, DISH_R, ui::BOX);
    g.circle(DISH_X, DISH_Y, DISH_R - 1, ui::BOX);
    g.circle(DISH_X, DISH_Y, 18, ui::BOX);
    g.hline(DISH_X - DISH_R + 6, DISH_Y, DISH_R * 2 - 12, ui::BOX);
    g.vline(DISH_X, DISH_Y - DISH_R + 6, DISH_R * 2 - 12, ui::BOX);

    // One view for the whole screen: while measuring, the bubble goes through the SAME
    // measured map the spin stops and the ring marker use (mapped()) - drawing it through
    // the previously saved calibration instead put the ball and the marks in different
    // frames whenever that record was stale, and a mirrored old map walked the ball WITH
    // the roll and drowned it. INTRO previews what the games currently see; RESULT
    // previews the record about to be saved, through the real pipeline.
    wc::Tilt view;
    if (phase_ == INTRO || phase_ == RESULT) {
        wc::cal::applyPipeline(phase_ == RESULT ? found_ : wc::Input::calibration(), raw_, view);
    } else {
        view = mapped();
    }
    // A bubble rises to the high side, so this is what a game would see, negated.
    const float bx = -view.ax, by = -view.ay;
    constexpr float PX_PER_G = 420.0f;   // about 7 px per degree
    float px = bx * PX_PER_G, py = by * PX_PER_G;
    const float d = std::hypot(px, py), lim = float(DISH_R - 16);
    if (d > lim) px *= lim / d, py *= lim / d;
    const bool centred = d < 9.0f;
    g.fillCircle(DISH_X + int(px), DISH_Y + int(py), 13, centred ? ui::GO : ui::ACCENT);
    g.fillCircle(DISH_X + int(px) - 4, DISH_Y + int(py) - 4, 3, ui::TEXT);

    if (phase_ == TURN) {
        ringDots(g, 0, 360, ui::BOX);
        const float sign = turned_ < 0 ? -1.0f : 1.0f;
        ringDots(g, 0, sign * std::min(std::fabs(turned_), 360.0f), ui::VALUE);
        // The green zone is the real acceptance window (TURN_MIN..TURN_MAX): the mark at
        // 180 is only a hint - a flat watch's reading does not depend on the azimuth, so
        // demanding a precise half turn would be theater. Draw what is actually accepted.
        ringDots(g, sign * TURN_MIN, sign * TURN_MAX, ui::GO);
        if (win_.t > 0.35f) ringDots(g, 0, 360 * std::min(1.0f, win_.t / WINDOW_S), ui::GO);
    } else if (phase_ == FLAT1 || phase_ == AIM || phase_ == TILT) {
        ringDots(g, 0, 360, ui::BOX);
        if (win_.t > 0) ringDots(g, 0, 360 * std::min(1.0f, win_.t / AIM_S), ui::GO);
    } else if (phase_ == SPIN) {
        ringDots(g, 0, 360, ui::BOX);
        for (int k = 0; k < 4; k++) {   // the four marks; the next one is the target
            const float mark = k * SPIN_MARK;
            ringDots(g, mark - 4, mark + 4, k < stops_ ? ui::GO : k == stops_ ? ui::VALUE : ui::BOX);
        }
        // The bubble's own bearing on the ring: line it up with the target mark.
        const float r = (bubble_deg_ - 90.0f) * float(M_PI) / 180.0f;
        g.fillCircle(DISH_X + int(std::lround(std::cos(r) * RING_R)),
                     DISH_Y + int(std::lround(std::sin(r) * RING_R)), 5, ui::ACCENT);
        if (win_.t > 0.35f) ringDots(g, 0, 360 * std::min(1.0f, win_.t / WINDOW_S), ui::GO);
    } else if (phase_ == TILTUP) {
        ringDots(g, 0, 360, ui::BOX);
    }
}

}  // namespace console
