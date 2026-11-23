// The background refresher (docs/reference/CALIBRATION.md 8): while the watch rests - games idle,
// a nightstand overnight - collect still-window means, keep the newest per
// gravity-direction bin, refresh the gyro bias continuously and refit the accel model
// rarely, only when the refit is not worse than what is saved. Lives in the input task
// on the watch; the emulator pumps the same object from its frame step.
#pragma once

#include <atomic>

#include "engine/input.h"

namespace wc::cal {

class Autocal {
public:
    void feed(const Tilt &raw);   // one sample, at whatever rate the IMU is read
    void reset();                 // the manual flow just saved: collect against it, fresh
    static void setSave(void (*save)(const TiltCal &));

private:
    static constexpr int BINS = 72;        // 12 azimuth sectors x 6 tilt bands
    static constexpr int WINDOW_N = 500;   // samples per still window (~2 s at 250 Hz)
    static constexpr int MIN_BINS = 6;     // spread a refit needs to say anything
    static constexpr int NEW_BINS = 3;     // fresh coverage since the last refit
    static constexpr int64_t REFIT_EVERY_US = 5 * 60 * 1000000ll;    // attempts, not saves;
    // saves ride on accepted refits - the coverage gate is what keeps them rare

    void considerRefit(int64_t now);
    void pushBias();

    int n_ = 0;                    // samples in the running still window
    int w_exc_ = 0;                // samples over the strict rate gate; a window with any earns no bin
    float asum_[3] = {0, 0, 0}, gsum_[3] = {0, 0, 0};   // window sums, board frame
    float bias_[3] = {0, 0, 0};    // slow EMA on the still-window gyro means, board frame
    bool bias_seen_ = false;
    bool have_[BINS] = {};
    float pts_[BINS][3] = {};      // newest raw accel mean per bin, board frame
    int bins_ = 0, new_bins_ = 0;
    int64_t last_refit_us_ = 0;
    std::atomic<bool> reset_req_{false};
};

// The single instance: the input task feeds it, the console resets it after a manual
// save, and the emulator pumps it in place of the task.
Autocal &autocal();

}  // namespace wc::cal
