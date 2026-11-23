// Settings > CALIBRATE: measures this watch's motion sensor and its board->screen axis
// map, and saves the correction for every game. docs/reference/CALIBRATION.md:
//
//   1. AIM        nearly upright in hand, screen toward the user, top edge up
//                 -> map pose 1, and the
//                 mid-z reading that breaks the flat-rings degeneracy
//   2. FLAT UP    lay it flat, screen up, hands off             -> map pose 2 + reading
//   3. HALF TURN  spin half a turn, flat                        -> reading; SKIP here
//                 saves the old-model correction plus the map
//   4. TILT UP    lift it to ~45 degrees, screen toward you     -> off the table; the
//                 angle itself is free, any steady tilt will do
//   5. SPIN       quarter turns about the vertical - the        -> four readings spread
//                 bubble lines up with each ring mark            around a real ring: a
//                 FLAT spin would see nothing - gravity rides the spin axis - but a
//                 tilted one sweeps gravity through a circle of the tilt's radius
//   6. FIT        all points onto one sphere                     -> result, quality in deg
//
// Everything is measured on raw board-frame samples (Input::rawTilt) - the app never
// un-applies a correction, it fits the sensor directly and stores the result through the
// map it just measured.
#pragma once

#include "engine/calib_fit.h"
#include "engine/engine.h"
#include "engine/gestures.h"

namespace console {

class CalibrateApp : public wc::Game {
public:
    explicit CalibrateApp(wc::Game *back) : back_(back) {}
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override { return phase_ >= AIM && phase_ <= TILT; }

private:
    enum Phase { INTRO, AIM, FLAT1, TURN, TILTUP, SPIN, TILT, RESULT, FAIL };
    // A run of samples taken while the watch isn't moving.
    struct Window {
        int n = 0;
        float t = 0;
        float a[3] = {0, 0, 0}, g[3] = {0, 0, 0};   // sums
        void reset() { *this = Window{}; }
    };

    void setPhase(Phase p);
    bool sample(float dt, float still_dps, float still_g);   // true once the window is full
    void takeReading(int slot);
    void deriveMap();
    bool runFit();            // fit all points; false = ask for the tilt pose, or fail
    void finishOldModel(bool two_readings);   // SKIP: the pre-v2 math, plus the new map
    void leave(wc::Engine &e);
    wc::Tilt mapped() const;   // the raw sample through the measured map: what the UI gates on

    wc::Game *back_;
    wc::Gestures ges_;
    Phase phase_ = INTRO;
    bool full_ = true;
    int msg_ = 0, drawn_msg_ = -1;

    Window win_;
    bool armed_ = false;                // a tap confirmed the pose; only then does the still window run
    wc::Tilt raw_ = {};
    wc::cal::AxMap map_ = wc::cal::guessAxMap();
    bool map_ok_ = false;
    int map_attempts_ = 0;              // loose-pose retries before the flow gives up
    int map_why_ = 0;                   // the last rejection's AxMapWhy - the fail hint

    static constexpr int MAX_PTS = 9;   // aim, flat up, half turn, 4 stops, tilt + rescue tilt
    float pts_[MAX_PTS][3];             // raw accel means, board frame - the fit's points
    float gsum_[MAX_PTS][3];            // raw gyro means, board frame - the bias's
    int npts_ = 0;
    wc::cal::SphereCal fit_;            // the board-frame fit, kept for the bench lines
    float az_up_ = 0;                   // flat-up reading's screen z; the flip is its negation
    float first_gz_ = 0;                // screen z rate at the flat-up reading (half turn)
    float turned_ = 0;                  // half turn: degrees so far
    float bubble_deg_ = 0;              // spin: the bubble's bearing on the ring, degrees
    int stops_ = 0;                     // tilted-spin readings taken
    bool ticked_ = false;               // this quarter's mark has beeped
    bool tilt_asked_ = false;           // the fit already requested the 45-degree rescue
    float bench_t_ = 0;                 // the once-a-second bench log cadence
    const char *fail_line1_ = nullptr, *fail_line2_ = nullptr;   // which check said no
    wc::TiltCal found_;
};

}  // namespace console
