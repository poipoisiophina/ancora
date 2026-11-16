// ancora_deadzone.h
// Deadzone shape and response curve, shared by the logic dll, the panel and
// test\ancora_test.cpp.
//
// Deadzone shape: a stick that drifts one way needs a large deadzone only in that
// direction. The shape holds, per direction, a share (0..1) of the deadzone value,
// so <hand>_deadzone stays the size (the panel slider scales the whole shape) and
// <hand>_deadzone_shape the form. Empty = round. The Tune page measures it from
// where the stick comes to rest.
//
// Curve: output magnitude o (0..1, after deadzone and edge stretch) becomes
//   (1 - c) o + c o^3
// so 0 and full stay where they are and small pushes give less, for finer
// control near the center. c = 0 is off, 1 the strongest.

#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "ancora_edge.h"

const int   kDzSectors = 16;            // directions, from right counterclockwise
const float kDzShapeMin = 0.05f;        // smallest share stored for a direction
const float kDzRoundAbove = 0.8f;       // every share at or above this: keep it round
const float kDzMargin = 0.03f;          // added to the furthest rest point per direction
const float kDzFloor = 0.05f;           // deadzone toward directions it never rested in
const float kMaxCurve = 1.0f;

struct DeadzoneShape {
    bool  on = false;
    float r[kDzSectors] = {};
};

// share of the deadzone toward (x, y), linear between the two neighbouring directions
inline float dz_shape_at(const DeadzoneShape& s, float x, float y) {
    if (!s.on) return 1.0f;
    if (x == 0.0f && y == 0.0f) return 1.0f;
    int k, k1;
    float f;
    edge_pos(x, y, kDzSectors, k, k1, f);
    return s.r[k] * (1.0f - f) + s.r[k1] * f;
}

// the deadzone radius per direction for rest points (already centered when the
// stick is), before rounding: each point is covered in its own two neighbouring
// directions and one more on each side, since a stick that rests at some angle
// also rests a little either side of it; then the margin, and the floor elsewhere
inline void dz_from_points(const float* xs, const float* ys, int n, float out[kDzSectors]) {
    for (int k = 0; k < kDzSectors; ++k) out[k] = 0.0f;
    for (int i = 0; i < n; ++i) {
        float m = std::sqrt(xs[i] * xs[i] + ys[i] * ys[i]);
        if (!(m > 0.0f)) continue;
        int k, k1;
        float f;
        edge_pos(xs[i], ys[i], kDzSectors, k, k1, f);
        for (int d = -1; d <= 2; ++d) {
            int j = ((k + d) % kDzSectors + kDzSectors) % kDzSectors;
            if (m > out[j]) out[j] = m;
        }
    }
    for (int k = 0; k < kDzSectors; ++k) {
        out[k] = out[k] > 0.0f ? out[k] + kDzMargin : 0.0f;
        if (out[k] < kDzFloor) out[k] = kDzFloor;
    }
}

// shape for radii per direction and the deadzone value dz they are shares of;
// off (round) when every direction is close to dz anyway
inline DeadzoneShape dz_shape_from(const float radius[kDzSectors], float dz) {
    DeadzoneShape s;
    if (!(dz > 0.0f)) return s;
    bool round = true;
    for (int k = 0; k < kDzSectors; ++k) {
        float v = radius[k] / dz;
        v = v < kDzShapeMin ? kDzShapeMin : v > 1.0f ? 1.0f : v;
        s.r[k] = v;
        if (v < kDzRoundAbove) round = false;
    }
    s.on = !round;
    return s;
}

// kDzSectors comma separated shares; empty or anything else = off
inline DeadzoneShape dz_shape_parse(const char* text) {
    DeadzoneShape s;
    const char* p = text;
    int count = 0;
    while (*p == ' ') ++p;
    if (!*p) return DeadzoneShape{};
    for (;;) {
        char* end = nullptr;
        double d = std::strtod(p, &end);
        if (end == p || !(d >= 0.0) || d > 1.5 || count == kDzSectors) return DeadzoneShape{};
        s.r[count++] = d < kDzShapeMin ? kDzShapeMin : d > 1.0 ? 1.0f : (float)d;
        p = end;
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p != ',') return DeadzoneShape{};
        ++p;
        while (*p == ' ') ++p;
    }
    if (count != kDzSectors) return DeadzoneShape{};
    s.on = true;
    return s;
}

inline std::string dz_shape_format(const DeadzoneShape& s) {
    if (!s.on) return "";
    std::string out;
    char buf[16];
    for (int k = 0; k < kDzSectors; ++k) {
        std::snprintf(buf, sizeof(buf), k ? ",%.2f" : "%.2f", s.r[k]);
        out += buf;
    }
    return out;
}

// output magnitude o (0..1) through the curve c (0 = off)
inline float curve_apply(float c, float o) {
    if (!(c > 0.0f)) return o;
    if (c > kMaxCurve) c = kMaxCurve;
    return (1.0f - c) * o + c * o * o * o;
}
