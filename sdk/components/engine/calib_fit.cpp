#include "engine/calib_fit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wc::cal {

uint16_t packAxMap(const AxMap &m)
{
    // 9 bits really: the third sign bit lands at position 8. Packing into a uint8_t once
    // silently dropped sgn[2] and every map that needed a mirrored board z axis came back
    // from storage as a det -1 reflection - vertical aim inverted on the watch.
    return uint16_t(m.idx[0] | m.idx[1] << 2 | m.idx[2] << 4 | (m.sgn[0] < 0 ? 1 : 0) << 6 |
                    (m.sgn[1] < 0 ? 1 : 0) << 7 | (m.sgn[2] < 0 ? 1 : 0) << 8);
}

AxMap unpackAxMap(uint16_t packed)
{
    AxMap m;
    for (int i = 0; i < 3; i++) {
        m.idx[i] = (packed >> (2 * i)) & 3;
        m.sgn[i] = (packed & (1 << (6 + i))) ? -1.0f : 1.0f;
    }
    return m;
}

void applyAxMap(const float in[3], const AxMap &m, float out[3])
{
    for (int i = 0; i < 3; i++) out[i] = m.sgn[i] * in[m.idx[i]];
}

bool deriveAxMap(const float aim_down[3], const float flatup_down[3], AxMap &out, int *why)
{
    const auto no = [why](int r) { if (why) *why = r; return false; };
    float r0[3], r1[3];
    const float n0 = std::sqrt(aim_down[0] * aim_down[0] + aim_down[1] * aim_down[1] + aim_down[2] * aim_down[2]);
    const float n1 = std::sqrt(flatup_down[0] * flatup_down[0] + flatup_down[1] * flatup_down[1] +
                               flatup_down[2] * flatup_down[2]);
    if (n0 < 0.5f || n1 < 0.5f) return no(AXMAP_SHAKEN);   // shaken mid-window: the mean is not gravity
    for (int i = 0; i < 3; i++) r0[i] = aim_down[i] / n0, r1[i] = flatup_down[i] / n1;

    // Gram-Schmidt in board frame: a2 = the glass-up pose's component across the aim
    // pose's down, a3 = their cross product. Screen targets: +y at the aim pose, across
    // to -z glass-up. The screen rows fall out as x<-a3, y<-r0, z<-(-a2) - a LEFT-handed
    // basis, like the screen frame itself (+x right, +y down, +z out of the glass):
    // x = -(y x z), which is what puts the snapped map in the true mount's det = -1
    // family. The right-handed completion (x = +a3, the cross product's own sign) was
    // the original bug: it handed the snap the x-mirrored twin of the mount, both
    // calibration poses lie in the screen x = 0 plane and cannot see the difference,
    // and the first thing that could was rolling the watch - the bubble walked WITH the
    // turn instead of against it.
    const float d01 = r1[0] * r0[0] + r1[1] * r0[1] + r1[2] * r0[2];
    float a2[3] = {r1[0] - d01 * r0[0], r1[1] - d01 * r0[1], r1[2] - d01 * r0[2]};
    const float n2 = std::sqrt(a2[0] * a2[0] + a2[1] * a2[1] + a2[2] * a2[2]);
    if (n2 < 0.5f) return no(AXMAP_REPEATED);    // the second pose repeated the first one
    for (int i = 0; i < 3; i++) a2[i] /= n2;
    const float a3[3] = {r0[1] * a2[2] - r0[2] * a2[1],
                         r0[2] * a2[0] - r0[0] * a2[2],
                         r0[0] * a2[1] - r0[1] * a2[0]};
    const float rows[3][3] = {
        { a3[0],  a3[1],  a3[2]},                // screen x = -(y x z): the left-handed row
        { r0[0],  r0[1],  r0[2]},                // screen y
        {-a2[0], -a2[1], -a2[2]},                // screen z
    };
    // Assign screen rows to board axes globally, not greedily. A greedy per-row pick
    // fails at exactly the recline the user can hold: at ~45 degrees the aim pose's
    // vector lands equidistant between two board axes (measured: 0.69 vs 0.70), each row
    // grabs the same axis, and a perfectly good pair is refused as "skewed". At that
    // angle two permutations also score within a hair of each other, so the pose checks
    // are part of the choice: the best-scoring det = -1 permutation whose map actually
    // reads the two poses back as what they promised wins. det = -1, not +1: the chip's
    // frame is right-handed, the screen's (+x right, +y down, +z out of the glass) is
    // left-handed, so every real mount is a reflection. When this filter demanded +1 the
    // derivation returned the x-mirrored twin - both poses lie in the x = 0 plane, so the
    // twins tie there - and on the watch gravity walked the wrong way around a roll.
    struct Cand {
        int idx[3];
        float sgn[3], score;
    };
    Cand cands[6];
    int ncands = 0;
    static const int PERM[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {1, 0, 2}, {0, 2, 1}, {2, 1, 0}};
    for (int p = 0; p < 6; p++) {
        const int *perm = PERM[p];
        Cand c;
        c.score = 0;
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            const float v = rows[i][perm[i]];
            // 0.65: a recline up to ~49 degrees still assigns; the pose checks below
            // reject what should be rejected - this threshold only clears garbage.
            if (std::fabs(v) < 0.65f) ok = false;   // every axis must carry its row
            c.score += std::fabs(v);
            c.sgn[i] = v > 0 ? 1.0f : -1.0f;
            c.idx[i] = perm[i];
        }
        const int parity = (perm[1] - perm[0]) * (perm[2] - perm[0]) * (perm[2] - perm[1]);
        if (ok && int(c.sgn[0] * c.sgn[1] * c.sgn[2]) * parity < 0) cands[ncands++] = c;   // det = -1
    }
    // Highest score first; the pose checks decide between the near-ties.
    for (int pass = 0; pass < ncands; pass++) {
        int best = -1;
        for (int c = 0; c < ncands; c++)
            if (cands[c].score >= 0 && (best < 0 || cands[c].score > cands[best].score)) best = c;
        if (best < 0) break;   // unreachable today (scores are never negative), but keeps the indexing honest
        Cand c = cands[best];
        cands[best].score = -1;   // consumed
        AxMap m;
        for (int i = 0; i < 3; i++) m.idx[i] = uint8_t(c.idx[i]), m.sgn[i] = c.sgn[i];
        // The poses must read back as what they promised - and the aim pose's reclined z
        // must be strictly negative, which is what rules out the mirrored mount a
        // glass-down flat pose would otherwise bless (see the header comment).
        float as[3];
        applyAxMap(r0, m, as);
        if (std::fabs(as[1] - 1.0f) > 0.35f) continue;   // aim tilt off - try the next map
        if (as[2] > -0.1f) continue;                     // reclined toward the user
        applyAxMap(r1, m, as);
        if (std::fabs(as[2] + 1.0f) > 0.35f) continue;   // not screen-up flat
        out = m;
        return true;
    }
    // Every candidate that scored well failed a pose check: name the likeliest cause.
    return no(ncands == 0 ? AXMAP_SNAP : AXMAP_AIM);
}

// Gaussian elimination with partial pivoting on a symmetric 6x6; b comes in as the right
// side and leaves as the solution. Returns the smallest pivot over the largest - a
// condition proxy: a coverage that leaves a parameter combination free shows up as a
// pivot collapsing toward zero, however well the individual points look.
static void solve6(double a[6][6], double b[6], double &pivot_ratio)
{
    double biggest = 0, smallest = 1e300;
    for (int col = 0; col < 6; col++) {
        int pick = col;
        for (int r = col + 1; r < 6; r++)
            if (std::fabs(a[r][col]) > std::fabs(a[pick][col])) pick = r;
        const double p = a[pick][col];
        if (p == 0) {
            pivot_ratio = 0;
            return;
        }
        if (pick != col)
            for (int c = 0; c < 6; c++) std::swap(a[col][c], a[pick][c]), std::swap(b[col], b[pick]);
        biggest = std::max(biggest, std::fabs(p));
        smallest = std::min(smallest, std::fabs(p));
        for (int r = col + 1; r < 6; r++) {
            const double k = a[r][col] / p;
            for (int c = col; c < 6; c++) a[r][c] -= k * a[col][c];
            b[r] -= k * b[col];
        }
    }
    pivot_ratio = smallest / biggest;
    for (int r = 5; r >= 0; r--) {
        for (int c = r + 1; c < 6; c++) b[r] -= a[r][c] * b[c];
        b[r] /= a[r][r];
    }
}

bool fitSphere(const float (*pts)[3], int n, SphereCal &out, float &worst_deg)
{
    if (n < 6) return false;
    double p[6] = {0, 0, 0, 1, 1, 1};   // b[3], g[3]
    double lambda = 1e-3;
    double cost = 1e300;
    double pivot_ratio = 1;

    for (int iter = 0; iter < 60; iter++) {
        double jtj[6][6] = {}, jte[6] = {};
        double new_cost = 0;
        for (int i = 0; i < n; i++) {
            const float *a = pts[i];
            double d[3], gd2 = 0, row[6];
            for (int j = 0; j < 3; j++) d[j] = double(a[j]) - p[j], gd2 += p[3 + j] * p[3 + j] * d[j] * d[j];
            const double m = std::sqrt(gd2);
            if (m < 1e-4) continue;
            const double e = m - 1.0;
            new_cost += e * e;
            for (int j = 0; j < 3; j++) {
                row[j] = -p[3 + j] * p[3 + j] * d[j] / m;      // de/db_j: d = a - b shrinks
                row[3 + j] = p[3 + j] * d[j] * d[j] / m;       // de/dg_j
            }
            for (int r = 0; r < 6; r++) {
                jte[r] += row[r] * e;
                for (int c = 0; c < 6; c++) jtj[r][c] += row[r] * row[c];
            }
        }
        double damped[6][6], step[6];
        for (int r = 0; r < 6; r++) {
            for (int c = 0; c < 6; c++) damped[r][c] = jtj[r][c];
            damped[r][r] *= 1.0 + lambda;
        }
        for (int r = 0; r < 6; r++) step[r] = -jte[r];
        solve6(damped, step, pivot_ratio);
        if (pivot_ratio <= 0) return false;   // singular even damped: the points cannot say
        if (new_cost >= cost) {
            lambda *= 4;   // overshooting: damp harder and try the same neighborhood again
            if (lambda > 1e8) break;
            continue;
        }
        cost = new_cost;
        double moved = 0;
        for (int r = 0; r < 6; r++) p[r] += step[r], moved += std::fabs(step[r]);
        if (moved < 1e-10) break;
    }

    // Coverage, not convergence, is what can sink this: a fit that ran to a standstill
    // on points that never pinned one parameter combination reports false, and the flow
    // asks for the one pose that breaks the tie instead of saving a confident guess.
    double probe[6][6] = {}, rhs[6] = {1, 0, 0, 0, 0, 0};
    // Rebuild JtJ at the solution (the loop's last one may have been a rejected step).
    for (int i = 0; i < n; i++) {
        const float *a = pts[i];
        double d[3], gd2 = 0, row[6];
        for (int j = 0; j < 3; j++) d[j] = double(a[j]) - p[j], gd2 += p[3 + j] * p[3 + j] * d[j] * d[j];
        const double m = std::sqrt(gd2);
        if (m < 1e-4) continue;
        for (int j = 0; j < 3; j++) {
            row[j] = p[3 + j] * p[3 + j] * d[j] / m;
            row[3 + j] = p[3 + j] * d[j] * d[j] / m;
        }
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 6; c++) probe[r][c] += row[r] * row[c];
    }
    solve6(probe, rhs, pivot_ratio);
    if (pivot_ratio < 1e-7) return false;

    for (int j = 0; j < 3; j++) {
        out.offset[j] = float(p[j]);
        out.gain[j] = float(p[3 + j]);
        // A fit that wandered off the physical range did not converge onto a sensor.
        if (out.gain[j] < 0.5f || out.gain[j] > 2.0f || std::fabs(out.offset[j]) > 0.5f) return false;
    }
    worst_deg = 0;
    for (int i = 0; i < n; i++) {
        double d[3], gd2 = 0;
        for (int j = 0; j < 3; j++) d[j] = double(pts[i][j]) - out.offset[j], gd2 += out.gain[j] * out.gain[j] * d[j] * d[j];
        worst_deg = std::max(worst_deg, float(std::fabs(std::sqrt(gd2) - 1.0)));
    }
    worst_deg = std::asin(std::min(1.0f, worst_deg)) * 57.29578f;
    return true;
}

void storeSphereCal(const SphereCal &board, const float bias_board[3], const AxMap &m, TiltCal &out, bool with_map)
{
    // screen = m * board, so a screen-axis parameter is its board-axis source through the
    // map: gains permute (a sign would square away), offsets and bias carry the sign.
    for (int i = 0; i < 3; i++) {
        out.gain[i] = board.gain[m.idx[i]];
        out.offset[i] = m.sgn[i] * board.offset[m.idx[i]];
        out.bias[i] = m.sgn[i] * bias_board[m.idx[i]];
    }
    if (with_map) {
        out.has_map = 1;
        out.axmap = packAxMap(m);
    }
}

void applyPipeline(const TiltCal &cal, const Tilt &raw, Tilt &out)
{
    const AxMap m = cal.has_map ? unpackAxMap(cal.axmap) : guessAxMap();
    const float ain[3] = {raw.ax, raw.ay, raw.az}, gin[3] = {raw.gx, raw.gy, raw.gz};
    float a[3], g[3];
    applyAxMap(ain, m, a);
    applyAxMap(gin, m, g);
    // Offsets first, then the scale: the offset is an error in the reading, so it has to
    // come off before the reading is stretched to where 1 g really is.
    out.ax = (a[0] - cal.offset[0]) * cal.gain[0];
    out.ay = (a[1] - cal.offset[1]) * cal.gain[1];
    out.az = (a[2] - cal.offset[2]) * cal.gain[2];
    // The gyro chain (rate, its integral and the bias that comes off both) reaches games
    // in screen axes with one locked asymmetry, pinned by hardware A/B on the real mount:
    // the pitch (screen x) and roll (screen z) components pass through as the true
    // screen-frame rotation, but the yaw component (screen y) arrives INVERTED - positive
    // yaw is a physical turn LEFT (the emulator's yaw convention), so heading math that
    // integrates the rate reads the mirrored direction unless it flips this one component;
    // flipping the whole mapped frame instead mirrors the pitch. The bias rides inside the
    // flip: it is stored as the plain
    // mapped board bias, so negating (mapped - bias) negates both consistently.
    out.gx = g[0] - cal.bias[0];
    out.gy = -(g[1] - cal.bias[1]);
    out.gz = g[2] - cal.bias[2];
    // The frame rotation integrates through the same map (a signed permutation is linear,
    // so integrating before or after it is the same numbers), and the bias comes off over
    // the time the integral really spans: a biased part that turned nowhere still
    // integrates bias*dt of phantom turn.
    const float rin[3] = {raw.rx, raw.ry, raw.rz};
    float r[3];
    applyAxMap(rin, m, r);
    out.rx = r[0] - cal.bias[0] * raw.rdt;
    out.ry = -(r[1] - cal.bias[1] * raw.rdt);
    out.rz = r[2] - cal.bias[2] * raw.rdt;
    out.rdt = raw.rdt;
}

}  // namespace wc::cal
