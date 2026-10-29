// ancora_logic.cpp
// Hot swappable ancora behaviour, loaded by driver_ancora.dll. Tracks each
// hand's /input/joystick/{x,y,touch} components and rewrites stick values per
// ancora.ini: touch gate (thumb off the cap -> 0,0) and an optional radial
// deadzone. Publishes live state to shared memory for the ancora panel.
// Rebuild this dll while SteamVR runs; the loader swaps it in.

#include <openvr_driver.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include "ancora_host_api.h"
#include "ancora_state.h"

using namespace vr;

namespace {

const AncoraHost* H = nullptr;

// per hand values: index 0 left, 1 right
struct Config {
    bool  enable        = false;           // master switch; false passes everything through
    bool  hand[2]       = {true, false};   // correct this stick at all
    bool  touch_gate[2] = {true, true};
    float deadzone[2]   = {0.0f, 0.0f};
};

struct Stick {
    uint64_t hx = 0, hy = 0, ht = 0;   // 0 = not found
    float raw_x = 0, raw_y = 0;
    bool  touch = false;
    void* iface = nullptr;
    uint64_t last_ms = 0;               // last update from the controller driver
};

const char* kHand[2] = {"left", "right"};
std::mutex g_mu;
Config g_cfg;
Stick g_s[2];
std::string g_ini;
FILETIME g_ini_time{};
uint64_t g_last_check = 0;
uint32_t g_updates[2] = {0, 0};
std::string g_model[2];

const uint64_t kConfigPollMs = 250;
const float kMaxDeadzone = 0.95f;

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

float output(const Stick& s, int i, bool want_x) {
    float x = s.raw_x, y = s.raw_y;
    if (g_cfg.enable && g_cfg.hand[i]) {
        // no touch component found -> never gate on touch, so the stick cannot lock
        if (g_cfg.touch_gate[i] && s.ht && !s.touch) return 0.0f;
        float m = std::sqrt(x * x + y * y);
        float dz = g_cfg.deadzone[i];
        if (dz > 0.0f && dz < 1.0f) {
            if (m < dz) return 0.0f;
            float k = (m - dz) / (1.0f - dz) / m;
            x *= k; y *= k;
        }
    }
    return want_x ? x : y;
}

// copy current state to shared memory for the panel; g_mu held
void publish() {
    if (!g_state) return;
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
        h.raw_x = s.raw_x; h.raw_y = s.raw_y;
        h.out_x = output(s, i, true); h.out_y = output(s, i, false);
        h.touch = s.touch;
        h.tracked = (s.hx ? 1u : 0u) | (s.hy ? 2u : 0u) | (s.ht ? 4u : 0u);
        bool moving = s.raw_x != 0.0f || s.raw_y != 0.0f;
        h.gated = moving && h.out_x == 0.0f && h.out_y == 0.0f;
        h.updates = g_updates[i];
        h.last_update_ms = s.last_ms;
        strncpy_s(h.model, g_model[i].c_str(), _TRUNCATE);
    }
    MemoryBarrier();
    g_state->seq = g_state->seq + 1;   // even: consistent
}

void open_state(uint32_t generation) {
    static_assert(sizeof(AncoraState) <= ANCORA_STATE_BYTES, "state must fit the section");
    g_state_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, ANCORA_STATE_BYTES,
                                     ANCORA_STATE_NAME);
    if (!g_state_map) { log("state shared memory failed " + std::to_string(GetLastError())); return; }
    g_state = (AncoraState*)MapViewOfFile(g_state_map, FILE_MAP_WRITE, 0, 0, ANCORA_STATE_BYTES);
    if (!g_state) { CloseHandle(g_state_map); g_state_map = nullptr; return; }
    g_state->magic = ANCORA_STATE_MAGIC;
    g_state->version = ANCORA_STATE_VERSION;
    g_state->generation = generation;
}

void close_state() {
    if (g_state) UnmapViewOfFile(g_state);
    if (g_state_map) CloseHandle(g_state_map);
    g_state = nullptr; g_state_map = nullptr;
}

// re-send both axes after a touch or config change; g_mu held
void repush(int i, double dt) {
    Stick& s = g_s[i];
    if (!s.iface || !s.hx || !s.hy) return;
    H->send_scalar(s.iface, s.hx, output(s, i, true), dt);
    H->send_scalar(s.iface, s.hy, output(s, i, false), dt);
}

// one deadzone key; missing or garbage -> 0, too large -> clamped
float read_deadzone(const char* file, const char* key) {
    char buf[32]{};
    GetPrivateProfileStringA("ancora", key, "0", buf, sizeof(buf), file);
    float v = (float)std::atof(buf);
    if (!(v >= 0.0f)) v = 0.0f;
    if (v > kMaxDeadzone) {
        log(std::string(key) + " " + buf + " clamped to " + std::to_string(kMaxDeadzone));
        v = kMaxDeadzone;
    }
    return v;
}

void load_config(bool force) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_ini.c_str(), GetFileExInfoStandard, &fa)) {
        if (force) log("no config at " + g_ini + ", disabled");
        return;
    }
    if (!force && CompareFileTime(&fa.ftLastWriteTime, &g_ini_time) == 0) return;
    g_ini_time = fa.ftLastWriteTime;
    const char* f = g_ini.c_str();
    Config c;
    c.enable  = GetPrivateProfileIntA("ancora", "enable", 0, f) != 0;
    c.hand[0] = GetPrivateProfileIntA("ancora", "left", 1, f) != 0;
    c.hand[1] = GetPrivateProfileIntA("ancora", "right", 0, f) != 0;
    for (int i = 0; i < 2; ++i) {
        std::string k = kHand[i];
        c.touch_gate[i] = GetPrivateProfileIntA("ancora", (k + "_touch_gate").c_str(), 1, f) != 0;
        c.deadzone[i]   = read_deadzone(f, (k + "_deadzone").c_str());
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_cfg = c;
    repush(0, 0); repush(1, 0);
    publish();
    char line[160];
    std::snprintf(line, sizeof(line),
                  "config enable=%d left=%d gate=%d dz=%.2f right=%d gate=%d dz=%.2f",
                  c.enable, c.hand[0], c.touch_gate[0], c.deadzone[0], c.hand[1], c.touch_gate[1], c.deadzone[1]);
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
        g_s[i].last_ms = GetTickCount64();
        if (*v != g_s[i].touch) {
            g_s[i].touch = *v;
            repush(i, dt);
            publish();
        }
    }
}

void on_scalar(void*, uint64_t h, float* v, double) {
    for (int i = 0; i < 2; ++i) {
        Stick& s = g_s[i];
        bool is_x = s.hx && h == s.hx, is_y = s.hy && h == s.hy;
        if (!is_x && !is_y) continue;
        std::lock_guard<std::mutex> lk(g_mu);
        (is_x ? s.raw_x : s.raw_y) = *v;
        *v = output(s, i, is_x);
        ++g_updates[i];
        s.last_ms = GetTickCount64();
        publish();
    }
}

void on_frame() {
    uint64_t now = GetTickCount64();
    if (now - g_last_check >= kConfigPollMs) {
        g_last_check = now;
        load_config(false);
        // heartbeat, so the panel can tell an idle stick from a stopped driver
        std::lock_guard<std::mutex> lk(g_mu);
        publish();
    }
}

void shutdown() {
    // hand the sticks back unfiltered so nothing stays parked at 0
    std::lock_guard<std::mutex> lk(g_mu);
    g_cfg.enable = false;
    repush(0, 0); repush(1, 0);
    publish();
    close_state();
    log("logic shutdown");
}

const AncoraLogic kLogic{ANCORA_API_VERSION, on_create, on_bool, on_scalar, on_frame, shutdown};

} // namespace

extern "C" __declspec(dllexport) const AncoraLogic* ancora_logic_init(const AncoraHost* host) {
    if (!host || host->version != ANCORA_API_VERSION) return nullptr;
    H = host;
    // config lives two levels above the driver root: %LOCALAPPDATA%\ancora\ancora.ini
    std::string top(host->driver_dir);
    for (int up = 0; up < 2; ++up) top = top.substr(0, top.find_last_of("\\/"));
    g_ini = top + "\\ancora.ini";
    log("config file " + g_ini);
    open_state(GetTickCount());
    load_config(true);
    return &kLogic;
}
