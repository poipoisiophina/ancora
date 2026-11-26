// ancora_pad.h
// Gamepads (Xbox, PS4, PS5 or any other pad Windows sees, wired or Bluetooth) whose
// stick takes over a controller's stick while it is pushed: the pure part, so
// test\ancora_test.cpp can run it. The logic dll reads the pads inside vrserver
// (ancora_pad_read.h), which needs no window focus, and writes the result into
// the controller's /input/joystick values, so a game in the background still
// gets it. Each hand picks one pad by its id, and which of its sticks to use;
// no pad drives a stick before it was picked, and every other pad is ignored.
// Pad ids: "xinput:<slot 0..3>", or "hid:<vid>:<pid>" (4 hex digits each, lower
// case) for a pad read as a HID device, with "#2", "#3", ... for a second or
// third pad of the same model. "joy:" (an earlier name for the same) reads as "hid:".

#pragma once
#include <cmath>
#include <cstring>
#include <string>

enum { PAD_OFF = 0, PAD_LEFT = 1, PAD_RIGHT = 2 };

const float kPadDeadzoneDefault = 0.15f;
const float kPadDeadzoneMax = 0.50f;
const int   kPadMax = 8;                // pads listed for the panel and selectable
const int   kPadIdLen = 24, kPadNameLen = 64;

inline const char* pad_name(int pad) { return pad == PAD_LEFT ? "left" : pad == PAD_RIGHT ? "right" : "off"; }

// one reading of a pad, axes -1..1, y up positive
struct PadReading {
    bool  connected = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    unsigned buttons = 0;       // one bit per button held, triggers included (the panel's pick by press)
};

// stick (x, y) through a radial deadzone, rescaled so the rim still gives full;
// true when it is pushed past the deadzone, and then (out_x, out_y) is the value
// games get in place of the controller's stick
inline bool pad_map(float dz, float x, float y, float& out_x, float& out_y) {
    out_x = out_y = 0.0f;
    if (!(dz >= 0.0f)) dz = 0.0f;
    if (dz > kPadDeadzoneMax) dz = kPadDeadzoneMax;
    float m = std::sqrt(x * x + y * y);
    if (!(m > dz)) return false;
    float o = (m - dz) / (1.0f - dz);
    if (o > 1.0f) o = 1.0f;
    out_x = x * o / m;
    out_y = y * o / m;
    return true;
}

// the chosen stick of a reading through pad_map; false when off, no pad or centered
inline bool pad_takeover(int pad, float dz, const PadReading& r, float& out_x, float& out_y) {
    out_x = out_y = 0.0f;
    if (pad == PAD_OFF || !r.connected) return false;
    return pad == PAD_LEFT ? pad_map(dz, r.lx, r.ly, out_x, out_y) : pad_map(dz, r.rx, r.ry, out_x, out_y);
}

// a raw axis from lo..hi to -1..1, the middle at 0
inline float pad_axis(unsigned v, unsigned lo, unsigned hi) {
    if (hi <= lo) return 0.0f;
    float a = ((float)v - (float)lo) / (float)(hi - lo) * 2.0f - 1.0f;
    return a < -1.0f ? -1.0f : a > 1.0f ? 1.0f : a;
}

// an XInput axis (-32768..32767) to -1..1
inline float pad_xinput_axis(short v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

// ---- Sony pads, read from their raw HID input reports ----------------------------
// Their Bluetooth reports 0x11 (DualShock 4) and 0x31 (DualSense) are not in the
// HID descriptor, and Steam switches the pads to them, so they are read by layout.
// Layouts (byte offsets, report id at 0):
//   DualShock 4  0x01 (USB, and Bluetooth simple)  sticks 1..4, buttons 5..7
//                0x11 (Bluetooth full)             sticks 3..6, buttons 7..9
//   DualSense    0x01 USB (64 byte reports)        sticks 1..4, buttons 8..10
//                0x01 Bluetooth simple             sticks 1..4, buttons 5..7
//                0x31 (Bluetooth full)             sticks 2..5, buttons 9..11
// Sticks are 0..255 with y growing downward. Buttons: the low nibble of the
// first byte is the d-pad (8 = released), the rest are one bit per button; the
// third byte of a DualShock 4 holds a counter above its two button bits.
enum { kSonyVid = 0x054C };

inline int pad_sony_model(unsigned vid, unsigned pid) {   // 4, 5, or 0 = not one
    if (vid != kSonyVid) return 0;
    if (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0) return 4;
    if (pid == 0x0CE6 || pid == 0x0DF2) return 5;
    return 0;
}

inline float pad_byte_axis(unsigned char v) { return pad_axis(v, 0, 255); }

// one input report r of n bytes; max_len is the device's input report length
// (64 on USB); false when the report is not one of the known layouts
inline bool pad_parse_sony(int model, const unsigned char* r, int n, int max_len, PadReading& out) {
    int st = -1, bt = -1;
    if (n < 10 || !model) return false;
    if (r[0] == 0x01) { st = 1; bt = model == 5 && max_len == 64 ? 8 : 5; }
    else if (r[0] == 0x11 && model == 4) { st = 3; bt = 7; }
    else if (r[0] == 0x31 && model == 5) { st = 2; bt = 9; }
    if (st < 0 || bt + 2 >= n) return false;
    out.connected = true;
    out.lx = pad_byte_axis(r[st]);
    out.ly = -pad_byte_axis(r[st + 1]);
    out.rx = pad_byte_axis(r[st + 2]);
    out.ry = -pad_byte_axis(r[st + 3]);
    unsigned dpad = r[bt] & 0x0F;
    out.buttons = (dpad < 8 ? 1u << dpad : 0u) | (r[bt] & 0xF0u) | (unsigned)r[bt + 1] << 8 |
                  (unsigned)(r[bt + 2] & (model == 4 ? 0x03 : 0x07)) << 16;
    return true;
}

// a device setting as read from the ini: a pad id, or "" (none picked) for
// anything that is not one
inline std::string pad_device_parse(const std::string& v) {
    std::string s;
    for (char c : v) s += c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    if (s.size() >= kPadIdLen) return "";
    if (s.rfind("xinput:", 0) == 0 && s.size() == 8 && s[7] >= '0' && s[7] <= '3') return s;
    if (s.rfind("joy:", 0) == 0) s = "hid:" + s.substr(4);
    if (s.rfind("hid:", 0) == 0 && s.size() >= 13 && s[8] == ':') {
        for (size_t i = 4; i < 13; ++i)
            if (i != 8 && !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return "";
        if (s.size() == 13) return s;
        if (s[13] == '#' && s.size() == 15 && s[14] >= '2' && s[14] <= '9') return s;
    }
    return "";
}

// index of the picked pad among n found ones (ids[k]); -1 = none picked, or the
// picked one is not connected
inline int pad_resolve(const char (*ids)[kPadIdLen], int n, const std::string& want) {
    if (want.empty()) return -1;
    for (int k = 0; k < n; ++k) if (want == ids[k]) return k;
    return -1;
}

// one pad stick moves one hand: true when the other hand already uses this pad's
// stick (both a picked pad and a stick set). When both hands claim the same stick
// (an ini edited by hand), the left hand keeps it
inline bool pad_taken(const std::string& dev, int stick, const std::string& other_dev, int other_stick) {
    return !dev.empty() && stick != PAD_OFF && dev == other_dev && stick == other_stick;
}

// the stick choice after `stick` for a hand, skipping the one the other hand uses:
// off, left, right, off
inline int pad_next_stick(int stick, const std::string& dev, const std::string& other_dev, int other_stick) {
    for (int step = 1; step <= 3; ++step) {
        int s = (stick + step) % 3;
        if (!pad_taken(dev, s, other_dev, other_stick)) return s;
    }
    return PAD_OFF;
}

// the stick a newly picked pad starts with: the hand's own side, else the other
// stick when the other hand uses that side of the same pad; a stick already set
// stays unless it is taken
inline int pad_first_stick(int hand, int stick, const std::string& dev, const std::string& other_dev, int other_stick) {
    if (dev.empty()) return PAD_OFF;
    if (stick != PAD_OFF && !pad_taken(dev, stick, other_dev, other_stick)) return stick;
    int own = hand ? PAD_RIGHT : PAD_LEFT, other = hand ? PAD_LEFT : PAD_RIGHT;
    return pad_taken(dev, own, other_dev, other_stick) ? other : own;
}

// index of the pad with a button pressed now that was not held before (before[k]),
// for picking a pad by pressing a button on it; -1 = none. The first such pad wins
inline int pad_pressed(const unsigned* before, const unsigned* now, int n) {
    for (int k = 0; k < n; ++k) if (now[k] & ~before[k]) return k;
    return -1;
}
