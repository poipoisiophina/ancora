// ancora_mouse.h
// The mouse stick: a joystick knob on the panel's Mouse stick page, dragged with
// the mouse, that takes over one controller's stick while it is held. The panel
// reads the mouse only inside its own window and writes the knob here; the logic
// dll reads it on its own thread and sends it as that controller's stick, so the
// game gets it whichever window is in front.
// Shared memory, single writer (the panel), seqlock: seq is odd while writing.
// The panel rewrites the block at least every frame while the knob is held; a
// block older than kMouseStaleMs counts as released, and so does a block left
// mid write for that long, so a closed, crashed or frozen panel never leaves a
// stick pushed. The logic checks what it reads (time, finite values, inside the
// unit circle), since any process of the session can write the block. The named
// event wakes the logic on a write.
// The plain functions below have no Windows API, for test\ancora_test.cpp.

#pragma once
#include <cmath>
#include <cstdint>

#define ANCORA_MOUSE_NAME  "Local\\ancora_mouse_v1"
#define ANCORA_MOUSE_EVENT "Local\\ancora_mouse_changed"
#define ANCORA_MOUSE_MAGIC 0x534D4341u   // "ACMS"
#define ANCORA_MOUSE_BYTES 4096          // section size; the struct must fit

struct AncoraMouse {
    uint32_t magic;
    volatile uint32_t seq;
    uint64_t tick_ms;           // GetTickCount64 of the last write
    int32_t  hand;              // stick the knob is held for: 0 left, 1 right, -1 released
    float    x, y;              // knob, -1..1 each, y up, inside the unit circle
};

const uint64_t kMouseStaleMs = 250;
// how far ahead of the logic's clock read a block may be stamped; both read the
// same system tick, so a block further ahead was not written by the panel
const uint64_t kMouseAheadMs = 50;

// (x, y) held at the unit circle when past it, direction kept; hypot, so a huge
// value keeps its direction instead of overflowing to 0,0
inline void mouse_rim(float& x, float& y) {
    float m = std::hypot(x, y);
    if (m > 1.0f) { x /= m; y /= m; }
}

// knob for a drag of (dx, dy) pixels from where it was grabbed, screen y down,
// on a pad of radius pixels: the offset as a share of the radius, y up, held at
// the rim when dragged past it
inline void mouse_knob(float dx, float dy, float radius, float& x, float& y) {
    x = y = 0.0f;
    if (!(radius > 0.0f)) return;
    x = dx / radius;
    y = -dy / radius;
    mouse_rim(x, y);
}

// whether a block written at tick by the panel holds the knob for hand i now:
// the panel's magic, this hand, finite values, and a time neither stale nor
// further ahead than kMouseAheadMs
inline bool mouse_holds(const AncoraMouse& m, int i, uint64_t now) {
    if (m.magic != ANCORA_MOUSE_MAGIC || m.hand != i) return false;
    if (!std::isfinite(m.x) || !std::isfinite(m.y)) return false;
    int64_t age = (int64_t)(now - m.tick_ms);
    return age >= -(int64_t)kMouseAheadMs && age <= (int64_t)kMouseStaleMs;
}

// no consistent read of the block since good (the panel is mid write, or stopped
// in one): whether a held knob is let go at now
inline bool mouse_read_lost(uint64_t good, uint64_t now) {
    return (int64_t)(now - good) > (int64_t)kMouseStaleMs;
}
