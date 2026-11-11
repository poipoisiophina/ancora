// ancora_config.h
// ancora.ini in one place: defaults, reading, the text of a new file, and
// changing single keys. Plain C++ on the file's text, no Windows API, so the
// logic dll, the panel and test\ancora_test.cpp all use the same code. File I/O
// (with a lock and an atomic swap) is in ancora_ini_io.h.
// Format: one [ancora] section, key=value lines, ';' comments. Keys are matched
// without regard to case, the first match wins, values are trimmed.

#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ancora_average.h"
#include "ancora_edge.h"

namespace ancfg {

const float kMaxDeadzone = 0.95f;
const char* const kHandKey[2] = {"left", "right"};

struct HandConfig {
    bool  enable = true;        // correct this stick at all
    bool  touch_gate = true;    // thumb off the cap -> 0,0
    float deadzone = 0.0f;      // radial, 0 = off
    int   average_ms = 0;       // moving average window, 0 = off
    EdgeReach edge;             // measured reach, off when the key is empty
    bool  edge_on = true;       // use the measured edge; off keeps the values for later
    bool  edge_bad = false;     // <hand>_edge had text that is not a valid reach list
    EdgeReach edge_base;        // furthest reach ever confirmed per direction, the floor
                                // for lowering by wear (ancora_edge.h); off when missing
};

struct Config {
    bool enable = true;         // master switch; a file without the key reads as off
    HandConfig hand[2];         // 0 left, 1 right
    std::string language = "auto";   // panel only, the driver ignores it
};

inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

inline bool same_nocase(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// one line of the file, split; kind: 0 other, 1 section header, 2 key=value
struct Line {
    int kind = 0;
    std::string name;           // section name or key
    std::string value;
};

inline Line parse_line(const std::string& raw) {
    Line l;
    std::string t = trim(raw);
    if (t.empty() || t[0] == ';' || t[0] == '#') return l;
    if (t[0] == '[') {
        size_t e = t.find(']');
        if (e != std::string::npos) { l.kind = 1; l.name = trim(t.substr(1, e - 1)); }
        return l;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) return l;
    l.kind = 2;
    l.name = trim(t.substr(0, eq));
    l.value = trim(t.substr(eq + 1));
    return l;
}

// calls f(line_start, line_end, Line, in_ancora) for every line; line_end excludes the newline
template <class F>
void each_line(const std::string& text, F f) {
    bool in = false;
    size_t a = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) a = 3;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        Line l = parse_line(text.substr(a, b - a));
        if (l.kind == 1) in = same_nocase(l.name, "ancora");
        if (!f(a, b, l, in)) return;
        if (b == text.size()) break;
        a = b + 1;
    }
}

// value of key in [ancora]; found = false when the key is missing
inline std::string get(const std::string& text, const char* key, bool* found = nullptr) {
    std::string v;
    bool got = false;
    each_line(text, [&](size_t, size_t, const Line& l, bool in) {
        if (in && l.kind == 2 && same_nocase(l.name, key)) { v = l.value; got = true; return false; }
        return true;
    });
    if (found) *found = got;
    return v;
}

// integer like GetPrivateProfileInt: leading number, else def when missing, 0 when garbage
inline int get_int(const std::string& text, const char* key, int def) {
    bool found;
    std::string v = get(text, key, &found);
    if (!found || v.empty()) return def;
    return (int)std::strtol(v.c_str(), nullptr, 10);
}

inline Config parse(const std::string& text) {
    Config c;
    c.enable = get_int(text, "enable", 0) != 0;
    for (int i = 0; i < 2; ++i) {
        HandConfig& h = c.hand[i];
        std::string k = kHandKey[i];
        h.enable = get_int(text, k.c_str(), 1) != 0;
        h.touch_gate = get_int(text, (k + "_touch_gate").c_str(), 1) != 0;
        float dz = (float)std::atof(get(text, (k + "_deadzone").c_str()).c_str());
        h.deadzone = !(dz >= 0.0f) ? 0.0f : dz > kMaxDeadzone ? kMaxDeadzone : dz;
        int ms = get_int(text, (k + "_average_ms").c_str(), 0);
        h.average_ms = ms < 0 ? 0 : ms > kMaxAverageMs ? kMaxAverageMs : ms;
        std::string edge = get(text, (k + "_edge").c_str());
        h.edge = edge_parse(edge.c_str());
        h.edge_bad = !edge.empty() && !h.edge.on;
        h.edge_on = get_int(text, (k + "_edge_on").c_str(), 1) != 0;
        h.edge_base = edge_parse(get(text, (k + "_edge_base").c_str()).c_str());
    }
    bool found;
    std::string lang = get(text, "language", &found);
    c.language = found && !lang.empty() ? lang : "auto";
    return c;
}

// edge in use by the driver: the measured one, unless switched off
inline EdgeReach effective_edge(const HandConfig& h) {
    EdgeReach e = h.edge;
    if (!h.edge_on) e.on = false;
    return e;
}

inline std::string num2(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// a whole new file with comments; the shipped driver\ancora\ancora.ini is this
// text for Config{} (test\ancora_test.cpp checks that)
inline std::string format(const Config& c) {
    std::string s =
        "; ancora driver config. The driver re-reads it within 250 ms while SteamVR runs.\r\n"
        "[ancora]\r\n"
        "; master switch, 0 = pass all input through unchanged\r\n"
        "enable=" + std::to_string(c.enable ? 1 : 0) + "\r\n"
        "; per hand: correct the stick (1 = on), thumb gate (thumb off the cap -> 0,0),\r\n"
        "; moving average for a shaky stick (0..300 ms, 0 = off, adds lag),\r\n"
        "; radial deadzone while touched (0..0.95, 0 = off),\r\n"
        "; edge stretch: " + std::to_string(kEdgeSectors) + " measured reach values, from right counterclockwise\r\n"
        "; (the Tune page writes them; empty = off), whether to use them (1 = on), and the\r\n"
        "; furthest reach seen per direction (the driver keeps it; wear never lowers the\r\n"
        "; stretch more than 0.10 below it)\r\n";
    for (int i = 0; i < 2; ++i) {
        const HandConfig& h = c.hand[i];
        std::string k = kHandKey[i];
        s += k + "=" + std::to_string(h.enable ? 1 : 0) + "\r\n";
        s += k + "_touch_gate=" + std::to_string(h.touch_gate ? 1 : 0) + "\r\n";
        s += k + "_average_ms=" + std::to_string(h.average_ms) + "\r\n";
        s += k + "_deadzone=" + num2(h.deadzone) + "\r\n";
        s += k + "_edge=" + edge_format(h.edge) + "\r\n";
        s += k + "_edge_on=" + std::to_string(h.edge_on ? 1 : 0) + "\r\n";
        s += k + "_edge_base=" + edge_format(h.edge_base) + "\r\n";
    }
    s += "; panel language: auto follows Windows, or a code such as de, ar, ja, zh-Hant\r\n";
    s += "language=" + c.language + "\r\n";
    return s;
}

// text with key set to value inside [ancora]: the existing line is replaced in
// place, else the key is added at the end of the section (or a new section is
// added). Every other line, comment and unknown key stays as it was.
inline std::string set(const std::string& text, const char* key, const std::string& value) {
    std::string nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    std::string line = std::string(key) + "=" + value;
    size_t hit_a = std::string::npos, hit_b = 0, section_end = std::string::npos;
    each_line(text, [&](size_t a, size_t b, const Line& l, bool in) {
        if (in && l.kind == 2 && same_nocase(l.name, key)) { hit_a = a; hit_b = b; return false; }
        if (in && (l.kind == 2 || l.kind == 1)) section_end = b;   // after the last line of the section so far
        return true;
    });
    if (hit_a != std::string::npos) {
        size_t b = hit_b;
        if (b > hit_a && text[b - 1] == '\r') --b;   // keep the line's own ending
        return text.substr(0, hit_a) + line + text.substr(b);
    }
    if (section_end != std::string::npos) {
        size_t b = section_end;
        if (b > 0 && text[b - 1] == '\r') --b;
        size_t end = text.find('\n', b);
        if (end == std::string::npos) return text + nl + line + nl;
        return text.substr(0, end + 1) + line + nl + text.substr(end + 1);
    }
    std::string out = text;
    if (!out.empty() && out.back() != '\n') out += nl;
    return out + "[ancora]" + nl + line + nl;
}

} // namespace ancfg
