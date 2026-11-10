// ancora_output.h
// What a game receives for one stick, as plain functions so test\ancora_test.cpp
// can run them: the thumb gate with its debounce and its fallback for a touch
// sensor that cannot be trusted, then the moving average, then the radial
// deadzone and edge stretch (ancora_edge.h). Used by the logic dll.

#pragma once
#include <cmath>
#include "ancora_edge.h"

// The cap sensor can flicker for a moment while the thumb stays on: the gate
// only closes once touch has read off for kGateReleaseMs.
const double kGateReleaseMs = 25.0;

// A stick held near its rim with touch reading off is almost always a thumb the
// sensor does not see (a dead or flaky sensor, a silicone cap, the side of the
// thumb), not drift. After kGateSuspectMs of that, the gate opens and stays open
// until the sensor reads touch again, so the stick still works.
const float  kGateSuspectReach = 0.9f;
const double kGateSuspectMs = 250.0;

struct TouchGate {
    bool   touch = false;
    double off_since = -1;      // when touch last went off, -1 = never on
    double far_since = -1;      // start of the current push at the rim while off, -1 = none
    bool   suspect = false;     // sensor not trusted until it reads touch again

    void on_touch(bool t, double now) {
        if (t) {
            touch = true;
            suspect = false;
            far_since = -1;
        } else if (touch) {
            touch = false;
            off_since = now;
        }
    }

    // raw stick magnitude; true when this reading made the sensor suspect
    bool on_stick(float m, double now) {
        if (touch || suspect || m < kGateSuspectReach) { far_since = -1; return false; }
        if (far_since < 0) { far_since = now; return false; }
        if (now - far_since < kGateSuspectMs) return false;
        suspect = true;
        return true;
    }

    bool closed(double now) const {
        if (touch || suspect) return false;
        return off_since < 0 || now - off_since >= kGateReleaseMs;
    }
};

struct StickSettings {
    bool  correct = false;      // master switch and this hand both on
    float deadzone = 0.0f;
    EdgeReach edge;             // in use, .on false when off
};

// output for raw (x, y): gate_closed -> 0,0; has_avg -> the average replaces the
// raw position; then the deadzone and edge stretch on the radial magnitude
inline void stick_output(const StickSettings& s, bool gate_closed, bool has_avg, float avg_x, float avg_y,
                         float raw_x, float raw_y, float& out_x, float& out_y) {
    out_x = raw_x;
    out_y = raw_y;
    if (!s.correct) return;
    if (gate_closed) { out_x = out_y = 0.0f; return; }
    float x = has_avg ? avg_x : raw_x, y = has_avg ? avg_y : raw_y;
    float dz = s.deadzone;
    if (!(dz > 0.0f && dz < 1.0f)) dz = 0.0f;
    if (dz > 0.0f || s.edge.on) {
        float m = std::sqrt(x * x + y * y);
        float o = edge_map(s.edge, x, y, m, dz);
        if (o <= 0.0f) { out_x = out_y = 0.0f; return; }
        x *= o / m;
        y *= o / m;
    }
    out_x = x;
    out_y = y;
}
