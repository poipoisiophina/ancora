// ancora_state.h
// Live state the logic dll publishes for the ancora panel (read only for the
// panel). Shared memory, single writer, seqlock: seq is odd while writing.

#pragma once
#include <cstdint>

#define ANCORA_STATE_NAME    "Local\\ancora_state_v1"
#define ANCORA_STATE_MAGIC   0x41434F52u   // "ACOR"
#define ANCORA_STATE_VERSION 3
#define ANCORA_STATE_BYTES   4096          // section size; the struct must fit

struct AncoraHandState {
    // config as applied to this hand
    uint32_t enable;            // correct this stick
    uint32_t touch_gate;        // thumb off the cap -> 0,0
    float    deadzone;          // radial, 0 = off
    // live values
    float    raw_x, raw_y;      // what the controller reports
    float    out_x, out_y;      // what games receive
    uint32_t touch;             // thumb on the stick cap
    uint32_t tracked;           // bit0 x, bit1 y, bit2 touch component found
    uint32_t gated;             // 1 while output is forced to 0 by the gate or deadzone
    uint32_t updates;           // count of stick updates, for a rate display
    char     model[64];         // controller model number
    uint64_t last_update_ms;    // GetTickCount64 of the last stick or touch update, 0 = never
};

struct AncoraState {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;
    uint32_t generation;        // bumps on every logic (re)load
    uint64_t tick_ms;           // GetTickCount64 of the last write
    uint32_t enable;            // master switch as applied
    AncoraHandState hand[2];    // 0 left, 1 right
};
