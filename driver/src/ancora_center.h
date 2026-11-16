// ancora_center.h
// Center offset: a worn stick that rests off center, measured on the Tune page,
// is moved back so its rest point reads 0,0. The shift is not a plain
// subtraction: every direction is rescaled so the rim stays the rim, so a full
// push still reads full in every direction, also toward the offset.
// Shared by the logic dll, the panel and test\ancora_test.cpp.
//
// For a rest point c (|c| < 1) and a reading p, d = p - c, u = d / |d|.
// Along u, the distance from c to the unit circle is
//   t = -(c . u) + sqrt((c . u)^2 - |c|^2 + 1)
// and the output is u |d| / t, capped at 1: c maps to 0, the rim to the rim.

#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

const float kMaxCenter = 0.50f;     // a rest point further out than this is clamped to it
const float kCenterMin = 0.02f;     // the Tune page does not center a stick closer than this

struct CenterOffset {
    bool  on = false;
    float x = 0.0f, y = 0.0f;
};

inline CenterOffset center_clamped(float x, float y) {
    CenterOffset c;
    if (!std::isfinite(x) || !std::isfinite(y)) return c;
    float m = std::sqrt(x * x + y * y);
    if (m > kMaxCenter) { x *= kMaxCenter / m; y *= kMaxCenter / m; }
    c.on = true;
    c.x = x;
    c.y = y;
    return c;
}

// the centered reading for (x, y); unchanged when c is off
inline void center_map(const CenterOffset& c, float x, float y, float& ox, float& oy) {
    ox = x;
    oy = y;
    if (!c.on) return;
    float dx = x - c.x, dy = y - c.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) { ox = oy = 0.0f; return; }
    float ux = dx / len, uy = dy / len;
    float b = c.x * ux + c.y * uy;
    float t = -b + std::sqrt(b * b - (c.x * c.x + c.y * c.y) + 1.0f);
    float m = t > 1e-6f ? len / t : 1.0f;
    if (m > 1.0f) m = 1.0f;
    ox = ux * m;
    oy = uy * m;
}

// "x,y"; empty or anything else = off
inline CenterOffset center_parse(const char* s) {
    const char* p = s;
    while (*p == ' ') ++p;
    if (!*p) return CenterOffset{};
    char* end = nullptr;
    double x = std::strtod(p, &end);
    if (end == p) return CenterOffset{};
    p = end;
    while (*p == ' ') ++p;
    if (*p != ',') return CenterOffset{};
    ++p;
    double y = std::strtod(p, &end);
    if (end == p) return CenterOffset{};
    p = end;
    while (*p == ' ') ++p;
    if (*p) return CenterOffset{};
    return center_clamped((float)x, (float)y);
}

inline std::string center_format(const CenterOffset& c) {
    if (!c.on) return "";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f", c.x, c.y);
    return buf;
}
