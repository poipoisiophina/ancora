// ancora_edge.h
// Edge stretch for a stick that cannot reach full travel in some directions.
// The reach is kept for kEdgeSectors directions, sector k centred on k * 360 / kEdgeSectors
// degrees counterclockwise from +x (right), and interpolated between them. The
// output magnitude is (m - dz) / (reach - dz), clamped to 0..1, so the deadzone
// still works in raw stick units (as the tune page measures it) and the measured
// reach in every direction maps to full output. Used by the logic dll and the panel.
// 64 directions (5.6 deg apart): a worn spot is often narrow, and with wider
// steps a strong reading just beside it would also raise the worn spot itself.

#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

const int kEdgeSectors = 64;
const int kEdgeOldSectors = 16;         // older ini lines; read and spread over kEdgeSectors
const float kEdgeMinReach = 0.3f;       // a reach below this is a measuring error
const float kEdgeMinSpan = 0.05f;       // reach - deadzone never below this
const float kEdgeCountFrom = 0.4f;      // stick readings below this never count as reach
const int kEdgePasses = 2;              // separate passes through each direction, at least
const float kEdgeSettle = 0.015f;       // a pass that pushes a direction further than this is not settled

struct EdgeReach {
    bool  on = false;
    float r[kEdgeSectors] = {};
};

// sector position of (x, y): k and k + 1 around it, f = 0 at k .. 1 at k + 1
inline void edge_pos(float x, float y, int n, int& k, int& k1, float& f) {
    double a = std::atan2((double)y, (double)x) * 180.0 / 3.14159265358979;
    if (a < 0) a += 360.0;
    double s = a / (360.0 / n);
    k = (int)s % n;
    k1 = (k + 1) % n;
    f = (float)(s - std::floor(s));
}

// reach toward (x, y), linear between the two neighbouring sectors
inline float edge_reach(const EdgeReach& e, float x, float y) {
    if (!e.on) return 1.0f;
    int k, k1;
    float f;
    edge_pos(x, y, kEdgeSectors, k, k1, f);
    return e.r[k] * (1.0f - f) + e.r[k1] * f;
}

// A stick does not stop at the same place on every push, and learning keeps the
// furthest push ever seen. Full output comes kEdgeOuter short of that reach, so an
// ordinary full push still gives full. Only with edge stretch on.
const float kEdgeOuter = 0.04f;

// output magnitude for raw magnitude m toward (x, y); dz = radial deadzone (0 = off)
inline float edge_map(const EdgeReach& e, float x, float y, float m, float dz) {
    if (m <= dz || m <= 0.0f) return 0.0f;
    float r = edge_reach(e, x, y);
    if (e.on) r = dz + (r - dz) * (1.0f - kEdgeOuter);
    if (r < dz + kEdgeMinSpan) r = dz + kEdgeMinSpan;
    float o = (m - dz) / (r - dz);
    return o > 1.0f ? 1.0f : o;
}

// learning during play: a stick seen further out than the stored reach raises that
// one direction to the reading, and only when the reading lies within
// kEdgeLearnWindow of the direction's own angle. Raising neighbours to fit a
// reading between two directions would push up a narrow worn spot from readings
// on its slopes, and since reach only grows, that error would stay. Readings come
// about a thousand a second during play, so every direction is hit squarely soon
// enough. Reach only grows, so the stretch only gets gentler. Shortfalls under
// kEdgeLearnMin are ignored (ini rounding, noise at the rim).
const float kEdgeLearnMin = 0.005f;
const float kEdgeLearnWindow = 0.25f;   // share of the step between two directions

inline bool edge_learn(EdgeReach& e, float x, float y) {
    if (!e.on) return false;
    float m = std::sqrt(x * x + y * y);
    if (m > 1.0f) m = 1.0f;
    int k, k1;
    float f;
    edge_pos(x, y, kEdgeSectors, k, k1, f);
    int d;
    if (f <= kEdgeLearnWindow) d = k;
    else if (f >= 1.0f - kEdgeLearnWindow) d = k1;
    else return false;
    if (m - e.r[d] < kEdgeLearnMin) return false;
    e.r[d] = m;
    return true;
}

// learning from a stream of x/y pairs. A single reading never raises the reach: it
// takes two pairs in a row pointing the same way (within kEdgeLearnSpread), and
// the shorter of the two counts, so a one-off glitch at the rim teaches nothing.
const float kEdgeLearnSpread = 360.0f / kEdgeSectors * 2;   // degrees

struct EdgeLearner {
    bool  have = false;
    float px = 0, py = 0;

    bool feed(EdgeReach& e, float x, float y) {
        bool raised = false;
        float m = std::sqrt(x * x + y * y), pm = std::sqrt(px * px + py * py);
        if (have && m > kEdgeCountFrom && pm > kEdgeCountFrom) {
            float c = (x * px + y * py) / (m * pm);
            if (c >= std::cos(kEdgeLearnSpread * 3.14159265f / 180)) {
                float use = (pm < m ? pm : m) / m;
                raised = edge_learn(e, x * use, y * use);
            }
        }
        px = x; py = y; have = true;
        return raised;
    }
};

// ini text "0.99,0.99,...": kEdgeSectors values, or kEdgeOldSectors from an older
// version (spread out in straight lines); anything else -> off
inline EdgeReach edge_parse(const char* s) {
    float v[kEdgeSectors];
    int count = 0;
    const char* p = s;
    while (*p == ' ') ++p;
    if (!*p) return EdgeReach{};
    for (;;) {
        char* end = nullptr;
        double d = std::strtod(p, &end);
        if (end == p || !(d >= kEdgeMinReach) || d > 1.5 || count == kEdgeSectors) return EdgeReach{};
        v[count++] = d > 1.0 ? 1.0f : (float)d;
        p = end;
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p != ',') return EdgeReach{};
        ++p;
        while (*p == ' ') ++p;
    }
    EdgeReach e;
    if (count == kEdgeSectors) {
        for (int k = 0; k < kEdgeSectors; ++k) e.r[k] = v[k];
    } else if (count == kEdgeOldSectors) {
        const int step = kEdgeSectors / kEdgeOldSectors;
        for (int k = 0; k < kEdgeSectors; ++k) {
            int a = k / step, b = (a + 1) % kEdgeOldSectors;
            float f = (k % step) / (float)step;
            e.r[k] = v[a] * (1.0f - f) + v[b] * f;
        }
    } else {
        return EdgeReach{};
    }
    e.on = true;
    return e;
}

inline std::string edge_format(const EdgeReach& e) {
    if (!e.on) return "";
    std::string out;
    char buf[16];
    for (int k = 0; k < kEdgeSectors; ++k) {
        std::snprintf(buf, sizeof(buf), k ? ",%.3f" : "%.3f", e.r[k]);
        out += buf;
    }
    return out;
}

// sector with the shortest reach
inline int edge_shortest(const EdgeReach& e) {
    int best = 0;
    for (int k = 1; k < kEdgeSectors; ++k)
        if (e.r[k] < e.r[best]) best = k;
    return best;
}

// Records the furthest reading per fine bin while the stick is rolled around its
// rim. There is no fixed number of turns: it is done once every bin has been
// passed through kEdgePasses separate times and the latest pass through it did
// not push it further than kEdgeSettle, so every direction has really been
// pushed to its edge. A sloppy pass somewhere only means more circling there.
// A sector's reach is read from the fine bin at its centre, so a narrow dip is
// not hidden by the larger reach at the sector's sides.
const int kEdgeBins = 64;               // measuring bins, one per sector
const int kEdgeBinsPerSector = kEdgeBins / kEdgeSectors;
const int kEdgeAway = 4;                // bins away a direction must be left before it counts as passed again
const int kEdgeMaxGap = 8;              // widest run of unsettled bins (45 deg) that may be filled in to finish early

struct EdgeMeasure {
    float bin[kEdgeBins] = {};          // furthest reading, bin b centred on b * 360 / kEdgeBins
    float start[kEdgeBins] = {};        // bin's furthest reading before its latest pass
    int   passes[kEdgeBins] = {};
    bool  away[kEdgeBins];              // left far enough since the last pass
    int   cur = -1;                     // bin of the last reading, -1 = stick near the centre

    EdgeMeasure() { for (bool& a : away) a = true; }

    static int dist(int a, int b) {
        int d = a > b ? a - b : b - a;
        return d > kEdgeBins / 2 ? kEdgeBins - d : d;
    }

    void feed(float x, float y) {
        float m = std::sqrt(x * x + y * y);
        if (m < kEdgeCountFrom) { cur = -1; return; }
        double a = std::atan2((double)y, (double)x) * 180.0 / 3.14159265358979;
        if (a < 0) a += 360.0;
        int b = (int)std::lround(a / (360.0 / kEdgeBins)) % kEdgeBins;
        if (b != cur) {
            for (int k = 0; k < kEdgeBins; ++k)
                if (dist(k, b) > kEdgeAway) away[k] = true;
            if (away[b]) {              // a new pass, not jitter on a bin border
                away[b] = false;
                start[b] = bin[b];
                ++passes[b];
            }
            cur = b;
        }
        if (m > bin[b]) bin[b] = m > 1.0f ? 1.0f : m;
    }
    bool settled(int b) const { return passes[b] >= kEdgePasses && bin[b] - start[b] < kEdgeSettle; }
    int settled_count() const {
        int n = 0;
        for (int b = 0; b < kEdgeBins; ++b) n += settled(b);
        return n;
    }
    bool done() const { return settled_count() == kEdgeBins; }
    // reach at sector k so far, 0 = not reached: the centre bin, else the
    // nearest reached bins on both sides within the sector, interpolated
    float sector(int k) const {
        int c = k * kEdgeBinsPerSector;
        if (bin[c] > 0) return bin[c];
        int h = kEdgeBinsPerSector / 2;
        float lo = 0, hi = 0;
        int dlo = 0, dhi = 0;
        for (int d = 1; d <= h && !lo; ++d) { lo = bin[(c - d + kEdgeBins) % kEdgeBins]; dlo = d; }
        for (int d = 1; d <= h && !hi; ++d) { hi = bin[(c + d) % kEdgeBins]; dhi = d; }
        if (lo > 0 && hi > 0) return (lo * dhi + hi * dlo) / (dlo + dhi);
        return lo > 0 ? lo : hi;
    }
    int covered() const {
        int n = 0;
        for (int k = 0; k < kEdgeSectors; ++k) n += sector(k) > 0;
        return n;
    }
    // widest run of bins that are not settled, 0 = all settled, kEdgeBins = none settled
    int widest_gap() const {
        int first = -1;
        for (int b = 0; b < kEdgeBins && first < 0; ++b) if (settled(b)) first = b;
        if (first < 0) return kEdgeBins;
        int widest = 0, run = 0;
        for (int n = 1; n <= kEdgeBins; ++n) {
            if (settled((first + n) % kEdgeBins)) { if (run > widest) widest = run; run = 0; }
            else ++run;
        }
        return widest;
    }
    // can finish early: some directions a worn stick skips at full push are left out
    bool can_finish() const { return widest_gap() <= kEdgeMaxGap; }
    // reach from the settled bins; an unsettled bin is filled in a straight line between
    // the nearest settled bins on both sides, or keeps its own reading where that is further
    EdgeReach result() const {
        EdgeReach e;
        if (!can_finish()) return e;
        float fill[kEdgeBins];
        for (int b = 0; b < kEdgeBins; ++b) {
            if (settled(b)) { fill[b] = bin[b]; continue; }
            int lo = 1, hi = 1;
            while (!settled((b - lo + kEdgeBins) % kEdgeBins)) ++lo;
            while (!settled((b + hi) % kEdgeBins)) ++hi;
            float a = bin[(b - lo + kEdgeBins) % kEdgeBins], c = bin[(b + hi) % kEdgeBins];
            float v = (a * hi + c * lo) / (lo + hi);
            fill[b] = bin[b] > v ? bin[b] : v;
        }
        e.on = true;
        for (int k = 0; k < kEdgeSectors; ++k) {
            float r = fill[k * kEdgeBinsPerSector];
            e.r[k] = r < kEdgeMinReach ? kEdgeMinReach : r;
        }
        return e;
    }
};
