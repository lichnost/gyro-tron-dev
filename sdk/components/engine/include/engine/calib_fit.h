// The motion calibration's math, as pure functions: the axis map (which board axis plays
// which screen axis), the per-axis accel model, the fit that measures them, and the one
// call that applies both. Pure so the host tests can own it and the emulator can run the
// exact code the input task runs - Input itself is a thin shell around applyPipeline.
// Design: docs/reference/CALIBRATION.md.
#pragma once

#include <cstdint>

#include "engine/input.h"

namespace wc::cal {

// A signed axis permutation: screen axis i reads board axis idx[i], negated where
// sgn < 0. A chip mounting is one of these; so is the compile-time guess below.
struct AxMap {
    uint8_t idx[3];
    float sgn[3];
};

// The mount this board's hardware sessions recorded: screen y = -board x (the vertical
// - aim session), screen z = +board z (the recline check), screen x = -board y (the roll
// session - the bubble walked WITH the turn under the + sign). Note the map is a
// REFLECTION, det -1, and must be: the chip's axes are right-handed while the screen
// frame (+x right, +y down, +z out of the glass) is left-handed, so every real mount
// mirrors one axis. A det = +1 guess would be indistinguishable from this one in the
// derivation: both calibration poses lie in the x = 0 plane, so the x-mirrored twins
// read identically there and only rotation can tell them apart. This is only what runs
// until Settings > CALIBRATE measures the board.
inline AxMap guessAxMap() { return {{1, 0, 2}, {-1, -1, 1}}; }
inline AxMap identityAxMap() { return {{0, 1, 2}, {1, 1, 1}}; }

// 9 bits in a uint16_t: 3 axis indices (2 bits each) + 3 sign bits - the one packing the
// engine derives, stores and loads, so one format describes the mount everywhere. It
// must stay wider than 8 bits: a mount whose glass normal is the board's -z axis needs
// sgn[2] = -1, and truncating that bit mirrors the map into a reflection.
uint16_t packAxMap(const AxMap &m);
AxMap unpackAxMap(uint16_t packed);

// det of the signed permutation: -1 a reflection - what every real mount is, because the
// chip's frame is right-handed and the screen's (+x right, +y down, +z out of the glass)
// is left-handed. +1 is the impossible one: it mirrors in-plane motion, and gravity walks
// the wrong way around a rolling watch - a stored map with det +1 is refused on load.
inline int axMapDet(const AxMap &m)
{
    // The Vandermonde product's sign is the permutation sign; its magnitude (2 here) is
    // factorial noise, and callers want a true det: +1 or -1.
    const int parity = (m.idx[1] - m.idx[0]) * (m.idx[2] - m.idx[0]) * (m.idx[2] - m.idx[1]);
    return int(m.sgn[0] * m.sgn[1] * m.sgn[2]) * (parity < 0 ? -1 : 1);
}

void applyAxMap(const float in[3], const AxMap &m, float out[3]);

// Two still poses pin the full board->screen map: held like a gun - glass toward
// the user, the TOP TILTED AWAY - down reads as +y in screen axes with a strictly
// negative z (the recline); lying glass-up, as -z. The pose checks after the snap exist
// because a side-laid watch still
// reads 1 g, and glass-up vs glass-down flat poses are indistinguishable across the two
// handednesses - no confidence check separates them. The recline's sign does. The
// handedness rule itself (det = -1, see guessAxMap) is what picks between the x-mirrored
// twins the two poses otherwise leave tied. Returns false when a pose was held too
// loosely, repeated, or of the wrong handedness: never save a doubtful map, a wrong
// one mirrors the horizon. why, when given, names the check that said no - the calibrate
// screen turns it into a hint the user can act on:
enum AxMapWhy {
    AXMAP_OK = 0,        // map derived
    AXMAP_SHAKEN,        // a reading's norm is not 1 g - taken mid-move
    AXMAP_REPEATED,      // the two poses were near-parallel - pose 1 was not tilted
    AXMAP_SNAP,          // axes would not separate cleanly - a pose was rolled/skewed
    AXMAP_MIRROR,        // the pair implies a reflection - physically impossible mount
    AXMAP_AIM,           // pose 1's tilt is off - too steep or rolled
    AXMAP_RECLINE,       // pose 1 reclined toward the user - the top must tilt AWAY
    AXMAP_FLAT,          // pose 2 did not read as screen-up flat
};
bool deriveAxMap(const float aim_down[3], const float flatup_down[3], AxMap &out, int *why = nullptr);

// Per-axis accel model: a_corr[i] = gain[i] * (a_raw[i] - offset[i]). Six parameters
// instead of the old three (x/y offsets and one scalar scale) - the z offset this model
// exposes is exactly what lied about the flip-down poses before.
struct SphereCal {
    float offset[3];
    float gain[3];
};

// Fit points of |gain*(a - offset)| = 1: still-window means of the raw accelerometer,
// all in one frame, at least six of them and spread over the sphere. Gauss-Newton with
// Levenberg damping and a pivot check, so coverage that does not identify the model
// (everything near one direction, or only the two flat rings - see CALIBRATION.md 4 on
// the z degeneracy) returns false instead of a confident wrong answer. worst_deg is the
// largest per-point misfit read as a direction error - conservative: every gram of
// misfit counted as direction, none as scale.
bool fitSphere(const float (*pts)[3], int n, SphereCal &out, float &worst_deg);

// A model fit in board frame becomes a stored record through the map that carries it to
// screen axes: gains permute, offsets and bias permute with the map's signs. The record
// is what Input applies after the map, so this is the only correct way to store a
// board-frame fit. with_map=false leaves the record's map untouched - the background
// refresher refits the model but never changes the map (only the manual flow may).
void storeSphereCal(const SphereCal &board, const float bias_board[3], const AxMap &m, TiltCal &out,
                    bool with_map = true);

// The whole input pipeline in one call: map board frame to screen frame, then take the
// calibration off. The input task stores raw samples and snapshot() runs this; the
// emulator feeds its synthetic IMU through the same path, so fault injection tests the
// real correction and not a lookalike.
void applyPipeline(const TiltCal &cal, const Tilt &raw, Tilt &out);

}  // namespace wc::cal
