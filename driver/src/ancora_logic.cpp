// ancora_logic.cpp
// Hot swappable ancora behaviour, loaded by driver_ancora.dll. Tracks each
// hand's /input/joystick/{x,y,touch} components and rewrites stick values per
// ancora.ini, in this order (ancora_output.h): touch gate (thumb off the cap ->
// 0,0), optional moving average, optional radial deadzone and edge stretch.
// Publishes live state to shared memory for the ancora panel.
// Rebuild this dll while SteamVR runs; the loader swaps it in.

#include <openvr_driver.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include "ancora_host_api.h"
#include "ancora_state.h"
#include "ancora_average.h"
#include "ancora_edge.h"
#include "ancora_learn.h"
#include "ancora_output.h"
#include "ancora_config.h"
#include "ancora_ini_io.h"

using namespace vr;

namespace {

const AncoraHost* H = nullptr;

// per hand values: index 0 left, 1 right
struct Config {
    bool  enable        = false;           // master switch; false passes everything through
    bool  hand[2]       = {true, true};    // correct this stick at all
    bool  touch_gate[2] = {true, true};
    float deadzone[2]   = {0.0f, 0.0f};
    int   average_ms[2] = {0, 0};          // moving average window, 0 = off
    EdgeReach edge[2];                     // edge stretch in use, off unless measured and on
};

struct Stick {
    uint64_t hx = 0, hy = 0, ht = 0;   // 0 = not found
    float raw_x = 0, raw_y = 0;
    bool  touch = false;
    void* iface = nullptr;
    uint64_t last_ms = 0;               // last update from the controller driver
    StickPair pair;                     // whole x/y pairs (ancora_learn.h)
    EdgeLearnState learn;               // edge learning during play
    StickAverage avg;
    TouchGate gate;                     // debounce and suspect sensor (ancora_output.h)
    float sent_x = 0, sent_y = 0;       // last values games received
    bool  sent = false;                 // sent_x/sent_y hold real values
    bool  was_closed = false;           // gate state at the last push
};

const char* kHand[2] = {"left", "right"};
std::mutex g_mu;
Config g_cfg;
Stick g_s[2];
std::wstring g_ini;
std::string g_ini_utf8;
FILETIME g_ini_time{};
uint64_t g_last_check = 0;
uint64_t g_start_ms = 0;
bool g_missing_warned[2] = {false, false};
uint32_t g_updates[2] = {0, 0};
std::string g_model[2];

const uint64_t kConfigPollMs = 250;
const uint64_t kMissingWarnMs = 20000;   // no stick seen this long after start: say so once

HANDLE g_state_map = nullptr;
AncoraState* g_state = nullptr;

void log(const std::string& s) { H->log(s.c_str()); }

std::string str_prop(uint64_t c, ETrackedDeviceProperty p) {
    char buf[256]{};
    H->get_string_prop(c, p, buf, sizeof(buf));
    return buf;
}

// -1 not a hand controller, 0 left, 1 right
int hand_of(uint64_t c) {
    int32_t role = H->get_int_prop(c, Prop_ControllerRoleHint_Int32);
    if (role == TrackedControllerRole_LeftHand) return 0;
    if (role == TrackedControllerRole_RightHand) return 1;
    std::string id = str_prop(c, Prop_ModelNumber_String) + " " + str_prop(c, Prop_RegisteredDeviceType_String);
    if (id.find("Left") != std::string::npos) return 0;
    if (id.find("Right") != std::string::npos) return 1;
    return -1;
}

// GetTickCount64 steps every 15 ms or so, too coarse for the gate and the average
double now_precise_ms() {
    static const double ms_per_tick = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * ms_per_tick;
}

// no touch component found -> never gate on touch, so the stick cannot lock
bool gate_closed(const Stick& s, int i, double now) { return g_cfg.touch_gate[i] && s.ht && s.gate.closed(now); }

bool correcting(int i) { return g_cfg.enable && g_cfg.hand[i]; }

// feed the current raw position to the average; emptied while it is unused, so
// old positions never leak in when the gate opens; g_mu held
void update_average(Stick& s, int i, double now) {
    if (!correcting(i) || g_cfg.average_ms[i] <= 0 || gate_closed(s, i, now)) {
        s.avg.reset();
        return;
    }
    s.avg.add(s.raw_x, s.raw_y, now, g_cfg.average_ms[i]);
}

void output(const Stick& s, int i, double now, float& x, float& y) {
    StickSettings st;
    st.correct = correcting(i);
    st.deadzone = g_cfg.deadzone[i];
    st.edge = g_cfg.edge[i];
    stick_output(st, gate_closed(s, i, now), s.avg.count > 0, s.avg.avg_x(), s.avg.avg_y(), s.raw_x, s.raw_y, x, y);
}

// copy current state to shared memory for the panel; g_mu held
void publish() {
    if (!g_state) return;
    double now = now_precise_ms();
    g_state->seq = g_state->seq + 1;   // odd: write in progress
    MemoryBarrier();
    g_state->tick_ms = GetTickCount64();
    g_state->enable  = g_cfg.enable;
    for (int i = 0; i < 2; ++i) {
        const Stick& s = g_s[i];
        AncoraHandState& h = g_state->hand[i];
        h.enable = g_cfg.hand[i];
        h.touch_gate = g_cfg.touch_gate[i];
        h.deadzone = g_cfg.deadzone[i];
        h.average_ms = (uint32_t)g_cfg.average_ms[i];
        h.raw_x = s.raw_x; h.raw_y = s.raw_y;
        output(s, i, now, h.out_x, h.out_y);
        h.touch = s.touch;
        h.tracked = (s.hx ? 1u : 0u) | (s.hy ? 2u : 0u) | (s.ht ? 4u : 0u);
        bool moving = s.raw_x != 0.0f || s.raw_y != 0.0f;
        h.gated = moving && h.out_x == 0.0f && h.out_y == 0.0f;
        h.updates = g_updates[i];
        h.last_update_ms = s.last_ms;
        h.pair_seq = s.pair.seq; h.pair_x = s.pair.x; h.pair_y = s.pair.y;
        strncpy_s(h.model, g_model[i].c_str(), _TRUNCATE);
    }
    MemoryBarrier();
    g_state->seq = g_state->seq + 1;   // even: consistent
}

void open_state(uint32_t generation) {
    static_assert(sizeof(AncoraState) <= ANCORA_STATE_BYTES, "state must fit the section");
    g_state_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, ANCORA_STATE_BYTES,
                                     L"" ANCORA_STATE_NAME);
    if (!g_state_map) { log("state shared memory failed " + std::to_string(GetLastError())); return; }
    g_state = (AncoraState*)MapViewOfFile(g_state_map, FILE_MAP_WRITE, 0, 0, ANCORA_STATE_BYTES);
    if (!g_state) { CloseHandle(g_state_map); g_state_map = nullptr; return; }
    g_state->magic = ANCORA_STATE_MAGIC;
    g_state->version = ANCORA_STATE_VERSION;
    g_state->generation = generation;
    // a previous instance stopped mid write: start from a consistent (even) count
    if (g_state->seq & 1) g_state->seq = g_state->seq + 1;
}

void close_state() {
    if (g_state) UnmapViewOfFile(g_state);
    if (g_state_map) CloseHandle(g_state_map);
    g_state = nullptr; g_state_map = nullptr;
}

// re-send both axes, after a change that does not come with a stick update
// (touch, config, the gate's debounce running out, the average settling); g_mu held
void repush(int i, double dt, double now) {
    Stick& s = g_s[i];
    s.was_closed = gate_closed(s, i, now);
    if (!s.iface || !s.hx || !s.hy) return;
    float x, y;
    output(s, i, now, x, y);
    H->send_scalar(s.iface, s.hx, x, dt);
    H->send_scalar(s.iface, s.hy, y, dt);
    s.sent_x = x; s.sent_y = y; s.sent = true;
}

void load_config(bool force) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &fa)) {
        if (force) log("no config at " + g_ini_utf8 + ", disabled");
        return;
    }
    if (!force && CompareFileTime(&fa.ftLastWriteTime, &g_ini_time) == 0) return;
    std::string text;
    if (!ancfg::read_file(g_ini, text)) return;   // retried on the next poll
    g_ini_time = fa.ftLastWriteTime;
    ancfg::Config f = ancfg::parse(text);
    Config c;
    std::string edge_text[2];
    c.enable = f.enable;
    for (int i = 0; i < 2; ++i) {
        const ancfg::HandConfig& h = f.hand[i];
        c.hand[i] = h.enable;
        c.touch_gate[i] = h.touch_gate;
        c.deadzone[i] = h.deadzone;
        c.average_ms[i] = h.average_ms;
        c.edge[i] = ancfg::effective_edge(h);
        if (h.edge_bad) log(std::string(kHand[i]) + "_edge unreadable, edge stretch off");
        edge_text[i] = ancfg::get(text, (std::string(kHand[i]) + "_edge").c_str());
    }
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < 2; ++i) g_s[i].learn.reload(c.edge[i], g_cfg.edge[i], edge_text[i]);
    g_cfg = c;
    double now = now_precise_ms();
    for (int i = 0; i < 2; ++i) { update_average(g_s[i], i, now); repush(i, 0, now); }
    publish();
    char line[240];
    std::snprintf(line, sizeof(line),
                  "config enable=%d left=%d gate=%d avg=%dms dz=%.2f edge=%.2f right=%d gate=%d avg=%dms dz=%.2f edge=%.2f",
                  c.enable, c.hand[0], c.touch_gate[0], c.average_ms[0], c.deadzone[0],
                  c.edge[0].on ? c.edge[0].r[edge_shortest(c.edge[0])] : 0.0f,
                  c.hand[1], c.touch_gate[1], c.average_ms[1], c.deadzone[1],
                  c.edge[1].on ? c.edge[1].r[edge_shortest(c.edge[1])] : 0.0f);
    log(line);
}

// ---- callbacks ---------------------------------------------------------------
void on_create(int kind, uint64_t c, const char* name, uint64_t h, void* iface) {
    bool x = kind == ANCORA_SCALAR && std::strcmp(name, "/input/joystick/x") == 0;
    bool y = kind == ANCORA_SCALAR && std::strcmp(name, "/input/joystick/y") == 0;
    bool t = kind == ANCORA_BOOL && std::strcmp(name, "/input/joystick/touch") == 0;
    if (!x && !y && !t) return;
    int i = hand_of(c);
    if (i < 0) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Stick& s = g_s[i];
    if (x) s.hx = h;
    if (y) s.hy = h;
    if (t) s.ht = h;
    if (x || y) s.iface = iface;
    g_model[i] = str_prop(c, Prop_ModelNumber_String);
    publish();
    log(std::string("tracking ") + kHand[i] + " " + name + " of " + str_prop(c, Prop_ModelNumber_String));
}

void on_bool(void*, uint64_t h, bool* v, double dt) {
    for (int i = 0; i < 2; ++i) {
        if (!g_s[i].ht || h != g_s[i].ht) continue;
        std::lock_guard<std::mutex> lk(g_mu);
        Stick& s = g_s[i];
        s.last_ms = GetTickCount64();
        if (*v != s.touch) {
            double now = now_precise_ms();
            s.touch = *v;
            s.gate.on_touch(*v, now);
            update_average(s, i, now);
            repush(i, dt, now);
            publish();
        }
    }
}

// edge learning during play (ancora_edge.h); saved to the ini kEdgeSaveMs after
// the last change; g_mu held
void learn_edge(int i, double now) {
    if (!correcting(i) || !g_cfg.edge[i].on) return;
    g_s[i].learn.on_pair(g_cfg.edge[i], g_s[i].pair.x, g_s[i].pair.y, now);
}

// write learned reach to the ini once the stick has stopped changing it; the
// file change reloads the config, which then holds the same values
void save_learned(double now) {
    for (int i = 0; i < 2; ++i) {
        std::string text;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_s[i].learn.save_due(now)) continue;
            g_s[i].learn.dirty = false;
            text = edge_format(g_cfg.edge[i]);
        }
        if (text.empty()) continue;
        std::string key = std::string(kHand[i]) + "_edge";
        if (ancfg::set_key(g_ini, key.c_str(), text)) log(std::string(kHand[i]) + " edge learned during play, saved");
        else log(std::string(kHand[i]) + " edge save failed " + std::to_string(GetLastError()));
    }
}

void on_scalar(void*, uint64_t h, float* v, double dt) {
    for (int i = 0; i < 2; ++i) {
        Stick& s = g_s[i];
        bool is_x = s.hx && h == s.hx, is_y = s.hy && h == s.hy;
        if (!is_x && !is_y) continue;
        std::lock_guard<std::mutex> lk(g_mu);
        double now = now_precise_ms();
        (is_x ? s.raw_x : s.raw_y) = *v;
        if (s.pair.update(is_x, *v)) learn_edge(i, now);
        if (s.ht && s.gate.on_stick(std::sqrt(s.raw_x * s.raw_x + s.raw_y * s.raw_y), now))
            log(std::string(kHand[i]) + " stick held at the rim while the thumb sensor reads off: "
                "gate paused until the sensor reads touch again");
        update_average(s, i, now);
        float x, y;
        output(s, i, now, x, y);
        *v = is_x ? x : y;
        // the deadzone, edge stretch and gate depend on both axes: when this update
        // changed what the other axis should be, send that too, since a driver that
        // only sends changed axes would leave it stale
        float other = is_x ? y : x;
        float& other_sent = is_x ? s.sent_y : s.sent_x;
        if (s.iface && s.hx && s.hy && (!s.sent || other != other_sent)) {
            H->send_scalar(s.iface, is_x ? s.hy : s.hx, other, dt);
            other_sent = other;
        }
        (is_x ? s.sent_x : s.sent_y) = *v;
        s.sent = true;
        s.was_closed = gate_closed(s, i, now);
        ++g_updates[i];
        s.last_ms = GetTickCount64();
        publish();
    }
}

void on_frame() {
    double now = now_precise_ms();
    for (int i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> lk(g_mu);
        Stick& s = g_s[i];
        // keep the average moving when a driver only sends changes: a stick that
        // stopped must still settle on its real position
        if (s.avg.count) { update_average(s, i, now); repush(i, 0, now); }
        // the gate's release debounce ran out with no update from the controller
        else if (gate_closed(s, i, now) != s.was_closed) { update_average(s, i, now); repush(i, 0, now); publish(); }
    }
    uint64_t tick = GetTickCount64();
    if (tick - g_last_check >= kConfigPollMs) {
        g_last_check = tick;
        save_learned(now);
        load_config(false);
        std::lock_guard<std::mutex> lk(g_mu);
        // components a controller driver created before ancora loaded are never seen
        for (int i = 0; i < 2; ++i)
            if (!g_missing_warned[i] && !g_s[i].hx && tick - g_start_ms >= kMissingWarnMs) {
                g_missing_warned[i] = true;
                log(std::string("no ") + kHand[i] + " stick seen yet: the controller is off, or its driver "
                    "created its inputs before ancora loaded (restart SteamVR with the controller on)");
            }
        // heartbeat, so the panel can tell an idle stick from a stopped driver
        publish();
    }
}

void start(uint32_t generation) {
    g_start_ms = GetTickCount64();
    open_state(generation);
    load_config(true);
}

void shutdown() {
    // hand the sticks back unfiltered so nothing stays parked at 0
    std::lock_guard<std::mutex> lk(g_mu);
    g_cfg.enable = false;
    double now = now_precise_ms();
    for (int i = 0; i < 2; ++i) { g_s[i].avg.reset(); repush(i, 0, now); }
    publish();
    close_state();
    log("logic shutdown");
}

const AncoraLogic kLogic{ANCORA_API_VERSION, start, on_create, on_bool, on_scalar, on_frame, shutdown};

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

} // namespace

// checks the host and finds the config; touches no shared state (see start)
extern "C" __declspec(dllexport) const AncoraLogic* ancora_logic_init(const AncoraHost* host) {
    if (!host || host->version != ANCORA_API_VERSION) return nullptr;
    H = host;
    // config lives two levels above the driver root: %LOCALAPPDATA%\ancora\ancora.ini
    std::string top(host->driver_dir);
    for (int up = 0; up < 2; ++up) top = top.substr(0, top.find_last_of("\\/"));
    g_ini_utf8 = top + "\\ancora.ini";
    g_ini = widen(g_ini_utf8);
    log("config file " + g_ini_utf8);
    return &kLogic;
}
