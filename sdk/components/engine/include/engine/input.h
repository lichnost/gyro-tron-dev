// Input is sampled on core 0 independently of the frame rate (buttons ~500 Hz,
// IMU ~250 Hz, touch on interrupt). Games read a per-frame snapshot; short taps
// that start and end between two frames still produce pressed/released edges.
#pragma once

#include <cstdint>

namespace wc {

enum Button : uint32_t {
    BTN_A = 1 << 0,   // BOOT key (by the USB port): console "home"
    BTN_B = 1 << 1,   // PWR key: app's secondary action
};

struct Touch {
    bool down = false;
    bool pressed = false;    // went down since last frame
    bool released = false;   // went up since last frame
    int x = 0, y = 0;        // last known position (kept after release)
    int64_t t_us = 0;        // sample time of x/y
};

struct Tilt {
    // g, screen-aligned: +x right, +y down, +z out of the glass. Mind the handedness:
    // that frame is LEFT-handed (x cross y points INTO the glass), while the chip's is
    // right-handed - so every board->screen axis map is a reflection, det -1 (guessAxMap).
    float ax = 0, ay = 0, az = 1;
    float gx = 0, gy = 0, gz = 0;   // deg/s
    // The gyro integrated over the whole frame (deg) and the seconds it spans - the sum
    // of every IMU sample since the last snapshot, mapped and bias-corrected like gx.
    // An orientation filter must turn by THIS, not by gx*dt: the hand's speed changes
    // inside a frame, so one instantaneous sample stretched over dt misses most of a
    // sweep, and heading has no reference to correct the miss back.
    float rx = 0, ry = 0, rz = 0;
    float rdt = 0;
};

// Resting error of this watch's motion sensor, measured by Settings > CALIBRATE and
// taken off every sample, so "flat" and "still" mean the same thing in every game.
// The record (docs/reference/CALIBRATION.md) is a per-axis accel model plus the measured
// board->screen axis map. Everything is stored in the frame games see - screen axes,
// after the map - because that is the frame the correction is applied in. No version
// field: there is no migration path to keep alive (wrong-size blobs are ignored and
// the user re-runs a flow they rarely run).
struct TiltCal {
    float offset[3] = {0, 0, 0};   // g the sensor reads at true rest, per screen axis
    // What the accelerometer reads for 1 g, per axis. Parts are trimmed to a few percent
    // and one of ours is out by more than six, which no amount of offset can fix: gravity
    // has to come out as 1.0 or every threshold measured in g is quietly wrong. Per axis,
    // because a scale error that is not isotropic tilts the implied gravity more the
    // further the watch is from the pose it was calibrated in.
    float gain[3] = {1, 1, 1};
    float bias[3] = {0, 0, 0};     // deg/s the gyro reads while still, per screen axis
    float residual_deg = 0;        // worst reading of the fit that produced this record
    uint8_t has_map = 0;           // the axis map below was measured, not the guess
    uint16_t axmap = 0;            // packed board->screen map (wc::cal::packAxMap), 9 bits
    uint16_t auto_refits = 0;      // background refresher's accepted refits (provenance)
};

struct InputState {
    uint32_t held = 0;
    uint32_t pressed = 0;      // went down (instant; use for action buttons)
    uint32_t released = 0;
    uint32_t clicked = 0;      // short press, reported on release
    uint32_t long_press = 0;   // held 600 ms (reported once, while still held)
    uint32_t double_clicked = 0;   // second click within 400 ms (the clicks are reported too)
    Touch touch;
    Tilt tilt;
    uint32_t touch_hz = 0;   // measured touch sample rate while finger is down
    uint32_t imu_ok = 0, imu_err = 0;   // cumulative IMU read results
};

class Input {
public:
    bool init();
    // Called once per frame by the engine.
    void snapshot(InputState &out);
    // Stop touching I2C (before light sleep). Blocks until the input task is idle.
    void pause(bool paused);
    // Applies from the next snapshot on.
    static void setCalibration(const TiltCal &cal);
    static TiltCal calibration();
    // Raw board-frame sample: what the sensor said, before the axis map and the
    // correction. This is what calibration measures and what the background collector
    // (engine/autocal.h) eats.
    static Tilt rawTilt();
};

}  // namespace wc
