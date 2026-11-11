// ancora_panel.cpp
// ancora control panel: Win32 + Direct2D (software target), no web runtime.
// Three pages in a SteamOS style layout: Status (both controllers, per hand
// settings), Tune (measures drift from the live stick and suggests a deadzone)
// and Diagnostics. Edits ancora.ini (the driver re-reads it within 250 ms) and
// reads live stick state the logic dll publishes in shared memory. Redraws only
// when something changed, at most 30 times a second, and draws nothing while
// hidden in the tray. Keyboard: Tab or arrows move focus, Enter or Space
// selects, Left/Right adjust a focused slider, Esc goes back.
// Text comes from XML language files (ancora_lang.h). A right to left language
// mirrors the whole page: shapes through one transform, text by placing it at
// the mirrored spot and laying it out right to left. The stick plots never
// mirror, since their axes are physical directions.
//
// Usage:
//   ancora.exe                    open the panel
//   ancora.exe --tray             start hidden in the tray
//   ancora.exe --steamvr          started by SteamVR: tray, exit when SteamVR exits
//   ancora.exe --screenshot f.png [--fake] [--page status|tune|edge|diag] [--lang code] [--kbd KLID]
//                                 render one frame, exit (--kbd: key hints for another
//                                 keyboard layout, such as 00000407 German)
//                                 and list any text that does not fit its space
//   ancora.exe --icon-png f.png   write the 16/32/48/256 px icons side by side, exit
//   ancora.exe --icon-ico f.ico   write the app icon file that the build embeds, exit
//   ancora.exe --selftest         check ini read/write and the tune math, exit code 0 = pass
//   ancora.exe --check | --register | --unregister [--root dir] [--backup-dir dir]
//                                 SteamVR side of an install (ancora_steamvr.h), exit code
//                                 0 = done, 1 = SteamVR running or failed. --root is the
//                                 install folder (default %LOCALAPPDATA%\ancora), Steam
//                                 file backups go to --backup-dir (default
//                                 %TEMP%\ancora-backup, which outlives an uninstall)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <mmsystem.h>
#include "../../driver/src/ancora_state.h"
#include "../../driver/src/ancora_average.h"
#include "../../driver/src/ancora_edge.h"
#include "../../driver/src/ancora_config.h"
#include "../../driver/src/ancora_ini_io.h"
#include "ancora_lang.h"
#include "ancora_steamvr.h"
// written by build_driver.bat from AppVersion in packaging\ancora.iss
#if __has_include("ancora_version.h")
#include "ancora_version.h"
#endif
#ifndef ANCORA_VERSION
#define ANCORA_VERSION L"dev"
#endif

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "shell32")
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "winmm")

namespace {

// ---- palette (ancora brand) ---------------------------------------------------
D2D1_COLOR_F rgb(uint32_t c, float a = 1.0f) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a);
}
const uint32_t kBg = 0x0E141B, kNav = 0x0A0F14, kCard = 0x16202B, kSel = 0x1E2A36, kWell = 0x0E141B;
const uint32_t kGrid = 0x2A3644, kLine = 0x22303D;
const uint32_t kText = 0xE6EDF3, kMuted = 0xA9B6C2, kDim = 0x8B98A5, kRaw = 0xFF6B5A, kRawText = 0xFF8A7C;
const uint32_t kOut = 0x3DDC97, kOkWell = 0x12241D, kBrass = 0xF5C451, kWarn = 0xFF9F43, kWhite = 0xFFFFFF;

const float kW = 1280, kH = 662;            // layout size in DIPs
const float kNavW = 260, kFootH = 56;
const int kTrail = 120;                     // raw trail points per stick
const uint64_t kTrailMs = 2000;
const UINT WM_TRAY = WM_APP + 1;
const UINT kTimerFrame = 1, kTimerSlow = 2;
const UINT kFrameMs = 33;                   // at most 30 redraws a second

// ---- config (ancora.ini) ---------------------------------------------------------
// defaults, reading and key writes are shared with the driver (ancora_config.h,
// ancora_ini_io.h): every write is one locked read, edit, atomic swap
const wchar_t* kHandKey[2] = {L"left", L"right"};

using HandConfig = ancfg::HandConfig;

struct Config : ancfg::Config {
    std::wstring language = L"auto";   // panel only, the driver ignores it
    bool exists = false;
};

std::wstring g_ini;

std::wstring hand_key(int i, const wchar_t* suffix) { return std::wstring(kHandKey[i]) + suffix; }

std::string ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? (char)c : '?';
    return s;
}

Config read_config(const std::wstring& path) {
    Config c;
    std::string text;
    c.exists = ancfg::read_file(path, text);
    static_cast<ancfg::Config&>(c) = ancfg::parse(text);
    std::wstring lang(c.ancfg::Config::language.begin(), c.ancfg::Config::language.end());
    c.language = lang_code_ok(lang) ? lang : L"auto";
    return c;
}

void write_key(const std::wstring& path, const std::wstring& key, const std::wstring& value) {
    ancfg::set_key(path, ascii(key).c_str(), ascii(value));
}

void write_bool(const std::wstring& path, const std::wstring& key, bool v) { write_key(path, key, v ? L"1" : L"0"); }

void write_average(const std::wstring& path, int hand, int ms) {
    write_key(path, hand_key(hand, L"_average_ms"), std::to_wstring(ms));
}

void write_deadzone(const std::wstring& path, int hand, float v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%.2f", v);
    write_key(path, hand_key(hand, L"_deadzone"), buf);
}

// empty value = edge stretch off. A new measurement is also the new furthest reach
// (<hand>_edge_base), so wear can lower the stretch again from there
void write_edge(const std::wstring& path, int hand, const EdgeReach& e) {
    ancfg::set_edge(path, hand, e, e);
}

// full file with comments, used when the file is missing (never replaces one)
void write_config_file(const std::wstring& path, const Config& c) {
    ancfg::Config f = c;
    f.language = ascii(c.language);
    ancfg::update(path, [&](const std::string& t) { return t.empty() ? ancfg::format(f) : t; });
}

FILETIME file_time(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    return fa.ftLastWriteTime;
}

// ---- paths and files -----------------------------------------------------------------
std::wstring local_appdata() {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return buf;
}

std::string read_file_tail(const std::wstring& path, size_t max_bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    LONGLONG start = size.QuadPart > (LONGLONG)max_bytes ? size.QuadPart - (LONGLONG)max_bytes : 0;
    LARGE_INTEGER pos; pos.QuadPart = start;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    std::string out((size_t)(size.QuadPart - start), '\0');
    DWORD rd = 0;
    if (!out.empty()) ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    out.resize(rd);
    CloseHandle(h);
    return out;
}

std::string read_file(const std::wstring& path) { return read_file_tail(path, 1 << 20); }

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// SteamVR log from the "log" entry of %LOCALAPPDATA%\openvr\openvrpaths.vrpath,
// the file SteamVR itself keeps; empty when SteamVR is not set up
std::wstring steamvr_log_path() {
    svr::Paths p = svr::find_paths();
    return p.log.empty() ? L"" : p.log + L"\\vrserver.txt";
}

// an ancora driver folder among SteamVR's registered external drivers
bool driver_registered() {
    for (const std::string& d : svr::find_paths().external_drivers)
        if (svr::ends_with_nocase(d, "ancora\\driver\\ancora")) return true;
    return false;
}

bool process_running(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

// ---- live state from the driver ---------------------------------------------------------
struct Live {
    HANDLE map = nullptr;
    const AncoraState* view = nullptr;
    AncoraState snap{};
    bool valid = false;
    uint32_t last_updates[2] = {0, 0};
    float rate[2] = {0, 0};
    uint64_t rate_tick = 0;
};

Live g_live;
bool g_fake = false;

void live_poll() {
    if (g_fake) return;
    if (!g_live.view) {
        g_live.map = OpenFileMappingA(FILE_MAP_READ, FALSE, ANCORA_STATE_NAME);
        if (!g_live.map) { g_live.valid = false; return; }
        g_live.view = (const AncoraState*)MapViewOfFile(g_live.map, FILE_MAP_READ, 0, 0, sizeof(AncoraState));
        if (!g_live.view) { CloseHandle(g_live.map); g_live.map = nullptr; g_live.valid = false; return; }
    }
    AncoraState s{};
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t a = g_live.view->seq;
        if (a & 1) continue;
        std::memcpy(&s, (const void*)g_live.view, sizeof(s));
        MemoryBarrier();
        if (g_live.view->seq == a) { s.seq = a; break; }
    }
    g_live.valid = s.magic == ANCORA_STATE_MAGIC && s.version == ANCORA_STATE_VERSION;
    g_live.snap = s;
}

bool live_now(uint64_t now) { return g_live.valid && now - g_live.snap.tick_ms < 1500; }

// the Steam Link driver streams stick values continuously, so silence means the
// controller dropped out (battery, sleep); checked on the existing redraws only
const uint64_t kSilentMs = 5000;

bool connected(const AncoraHandState& h, uint64_t now) {
    return (h.tracked & 3) == 3 && h.last_update_ms && now - h.last_update_ms < kSilentMs;
}

// thumb sensor column: no sensor, ignored (the gate's fallback is on), on or off
Str thumb_label(const AncoraHandState& h) {
    if (!(h.tracked & 4)) return S_thumb_no_sensor;
    if (h.gate_suspect) return S_thumb_suspect;
    return h.touch ? S_thumb_on : S_thumb_off;
}
uint32_t thumb_color(const AncoraHandState& h) { return h.gate_suspect ? kWarn : h.touch ? kBrass : kMuted; }

void fake_state(uint64_t now) {
    AncoraState& s = g_live.snap;
    s.magic = ANCORA_STATE_MAGIC; s.version = ANCORA_STATE_VERSION;
    s.tick_ms = now; s.enable = 1; s.generation = 3;
    float t = (now % 100000) / 1000.0f;
    AncoraHandState& l = s.hand[0];
    l.enable = 1; l.touch_gate = 1; l.deadzone = 0.0f;
    l.raw_x = 0.16f + 0.04f * std::sin(t * 1.7f); l.raw_y = 0.38f + 0.05f * std::cos(t * 1.3f);
    l.out_x = 0; l.out_y = 0; l.touch = 0; l.tracked = 7; l.gated = 1; l.updates = (uint32_t)(t * 500);
    strcpy_s(l.model, "Oculus Quest3 (Left Controller)");
    l.last_update_ms = now;
    AncoraHandState& r = s.hand[1];
    r.enable = 1; r.touch_gate = 0; r.deadzone = 0.20f; r.average_ms = 100;
    r.raw_x = 0.62f * std::cos(t * 0.4f + 0.9f); r.raw_y = 0.62f * std::sin(t * 0.4f + 0.9f);
    r.out_x = r.raw_x * 0.775f; r.out_y = r.raw_y * 0.775f;   // (0.62 - 0.20) / 0.80 / 0.62
    r.touch = 1; r.tracked = 7; r.gated = 0; r.updates = (uint32_t)(t * 500);
    strcpy_s(r.model, "Oculus Quest3 (Right Controller)");
    r.last_update_ms = now;
    g_live.valid = true;
}

// ---- tune: drift measurement from the live stick ------------------------------------
struct Point { float x, y; };

struct TuneResult {
    float peak = 0, typical = 0, suggested = 0;
    Str direction = S_dir_none;
    bool gate_ok = false;       // thumb sensor stayed off while the stick was let go
    bool has_sensor = false;
};

// peak and median magnitude, main direction, deadzone suggestion rounded up to 5 %
TuneResult tune_compute(const std::vector<Point>& pts, int touch_on, int samples, bool sensor) {
    TuneResult r;
    r.has_sensor = sensor;
    r.gate_ok = sensor && samples > 0 && touch_on * 20 < samples;   // under 5 % of samples touched
    if (pts.empty()) return r;
    std::vector<float> mags;
    float sx = 0, sy = 0;
    for (const Point& p : pts) {
        mags.push_back(std::sqrt(p.x * p.x + p.y * p.y));
        sx += p.x; sy += p.y;
    }
    std::sort(mags.begin(), mags.end());
    r.peak = mags.back();
    r.typical = mags[mags.size() / 2];
    r.suggested = std::fmin(0.95f, std::ceil((r.peak + 0.03f) / 0.05f - 1e-4f) * 0.05f);
    if (std::fabs(sx) + std::fabs(sy) > 1e-3f) {
        float a = std::atan2(sy, sx) * 180.0f / 3.14159265f;   // 90 = forward (+y)
        static const Str names[8] = {S_dir_right, S_dir_forward_right, S_dir_forward, S_dir_forward_left,
                                     S_dir_left, S_dir_back_left, S_dir_back, S_dir_back_right};
        int k = (int)std::lround((a < 0 ? a + 360 : a) / 45.0f) % 8;
        r.direction = names[k];
    }
    return r;
}

// drift: IDLE -> REST -> FLICK -> REVIEW; edge stretch: IDLE -> EDGE -> EDGE_REVIEW
enum TuneStage { TUNE_IDLE, TUNE_REST, TUNE_FLICK, TUNE_REVIEW, TUNE_EDGE, TUNE_EDGE_REVIEW };
// an edge reach at or above this needs no stretch
const float kEdgeFine = 0.95f;
const uint64_t kRestMs = 5000;
const int kFlicks = 6;
// after a flick the stick never truly stops (it creeps and jitters off center),
// so its rest point is the average over a fixed window after release
const float kArm = 0.7f;                   // a push past this arms a flick
const uint64_t kSettleMs = 600, kAverageMs = 500;

struct Tune {
    int hand = 0;
    TuneStage stage = TUNE_IDLE;
    uint64_t t0 = 0, last_sample = 0, release_t = 0;
    std::vector<Point> pts;
    int touch_on = 0, samples = 0, flicks = 0;
    bool armed = false;
    float sum_x = 0, sum_y = 0;
    int sum_n = 0;
    TuneResult result;
    EdgeMeasure edge;           // edge stretch measurement
    EdgeReach edge_result;
};

Tune g_tune;

bool tune_measuring() {
    return g_tune.stage == TUNE_REST || g_tune.stage == TUNE_FLICK || g_tune.stage == TUNE_EDGE;
}

// The edge is measured from every stick update, not once per frame: at 30
// frames a second a quick circle leaves each direction one or two readings,
// often taken while the stick is still swinging out, which reads as a short
// reach. A sampler thread reads the shared state about once a millisecond
// (the driver publishes on every update) into g_edge_live; the UI thread
// copies it into g_tune.edge each frame.
std::mutex g_edge_mu;
EdgeMeasure g_edge_live;                // guarded by g_edge_mu
std::thread g_edge_thread;
std::atomic<bool> g_edge_run{false};

void edge_sampler(int hand) {
    HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, ANCORA_STATE_NAME);
    const AncoraState* view = map ? (const AncoraState*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(AncoraState)) : nullptr;
    timeBeginPeriod(1);
    uint32_t last_pair = 0;
    while (g_edge_run && view) {
        uint32_t a = view->seq;
        if (!(a & 1) && view->magic == ANCORA_STATE_MAGIC && view->version == ANCORA_STATE_VERSION) {
            const AncoraHandState& h = view->hand[hand];
            uint32_t ps = h.pair_seq;
            float x = h.pair_x, y = h.pair_y;   // a whole pair, never a new x with an old y
            MemoryBarrier();
            if (view->seq == a && ps != last_pair) {
                last_pair = ps;
                std::lock_guard<std::mutex> lk(g_edge_mu);
                g_edge_live.feed(x, y);
            }
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    if (view) UnmapViewOfFile(view);
    if (map) CloseHandle(map);
}

void edge_sampler_stop() {
    g_edge_run = false;
    if (g_edge_thread.joinable()) g_edge_thread.join();
}

// sampler: false in the self test, which feeds readings itself
void tune_start_edge(bool sampler = true) {
    edge_sampler_stop();
    int hand = g_tune.hand;
    g_tune = Tune{};
    g_tune.hand = hand;
    g_tune.stage = TUNE_EDGE;
    {
        std::lock_guard<std::mutex> lk(g_edge_mu);
        g_edge_live = EdgeMeasure{};
    }
    if (sampler && !g_fake) {
        g_edge_run = true;
        g_edge_thread = std::thread(edge_sampler, hand);
    }
}

void tune_finish_edge() {
    g_tune.edge_result = g_tune.edge.result();
    g_tune.stage = TUNE_EDGE_REVIEW;
}

// one stick reading while measuring the edge (self test); returns true when it is done
bool tune_feed_edge(float x, float y) {
    if (g_tune.stage != TUNE_EDGE) return false;
    g_tune.edge.feed(x, y);
    if (!g_tune.edge.done()) return false;
    tune_finish_edge();
    return true;
}

// UI thread, once a frame: take over what the sampler collected
void tune_pull_edge() {
    if (g_tune.stage != TUNE_EDGE || !g_edge_run) return;
    {
        std::lock_guard<std::mutex> lk(g_edge_mu);
        g_tune.edge = g_edge_live;
    }
    if (g_tune.edge.done()) {
        edge_sampler_stop();
        tune_finish_edge();
    }
}

// "Finish with these": the stick skips some directions; fill the small gaps in
void tune_finish_edge_early() {
    if (g_tune.stage != TUNE_EDGE) return;
    if (g_edge_run) {
        std::lock_guard<std::mutex> lk(g_edge_mu);
        g_tune.edge = g_edge_live;
    }
    if (!g_tune.edge.can_finish()) return;
    edge_sampler_stop();
    tune_finish_edge();
}

void tune_reset(int hand) {
    edge_sampler_stop();
    g_tune = Tune{};
    g_tune.hand = hand;
}

void tune_start() {
    int hand = g_tune.hand;
    tune_reset(hand);
    g_tune.stage = TUNE_REST;
    g_tune.t0 = g_tune.last_sample = GetTickCount64();
}

// one stick reading while measuring; returns true when something visible changed
bool tune_feed(float x, float y, bool touch, bool sensor, uint64_t now) {
    if (g_tune.stage == TUNE_REST) {
        ++g_tune.samples;
        if (touch) ++g_tune.touch_on;
        // a point every 100 ms on average, whatever the frame rate
        if (now - g_tune.last_sample >= 100) {
            g_tune.pts.push_back({x, y});
            g_tune.last_sample += 100;
            if (now - g_tune.last_sample >= 100) g_tune.last_sample = now;   // fell far behind
        }
        if (now - g_tune.t0 >= kRestMs) { g_tune.stage = TUNE_FLICK; return true; }
        return false;
    }
    if (g_tune.stage != TUNE_FLICK) return false;
    // a push past kArm (re)arms; after release wait kSettleMs, then average kAverageMs
    if (std::sqrt(x * x + y * y) > kArm) {
        g_tune.armed = true;
        g_tune.release_t = 0;
        g_tune.sum_x = g_tune.sum_y = 0;
        g_tune.sum_n = 0;
        return false;
    }
    if (!g_tune.armed) return false;
    if (!g_tune.release_t) g_tune.release_t = now;
    uint64_t dt = now - g_tune.release_t;
    if (dt < kSettleMs) return false;
    ++g_tune.samples;
    if (touch) ++g_tune.touch_on;
    if (dt < kSettleMs + kAverageMs) {
        g_tune.sum_x += x; g_tune.sum_y += y; ++g_tune.sum_n;
        return false;
    }
    if (g_tune.sum_n) g_tune.pts.push_back({g_tune.sum_x / g_tune.sum_n, g_tune.sum_y / g_tune.sum_n});
    g_tune.armed = false;
    g_tune.release_t = 0;
    if (++g_tune.flicks >= kFlicks) {
        g_tune.result = tune_compute(g_tune.pts, g_tune.touch_on, g_tune.samples, sensor);
        g_tune.stage = TUNE_REVIEW;
    }
    return true;
}

bool tune_sample(uint64_t now) {
    if (!tune_measuring()) return false;
    if (!live_now(now)) return false;
    const AncoraHandState& h = g_live.snap.hand[g_tune.hand];
    if (g_tune.stage == TUNE_EDGE) { tune_pull_edge(); return true; }   // the outline grows every frame
    return tune_feed(h.raw_x, h.raw_y, h.touch != 0, (h.tracked & 4) != 0, now);
}

// ---- log facts for diagnostics ----------------------------------------------------------
struct LogFacts {
    std::vector<std::wstring> lines;      // "hh:mm:ss  message", ancora lines only
    int tables = -1, generation = -1, replayed = -1;
    std::wstring gen_time, config_time;
};

// ---- app state ---------------------------------------------------------------------
struct TrailPoint { float x, y; uint64_t t; };
struct Trail { TrailPoint p[kTrail]; int head = 0, count = 0; };

enum Page { PAGE_STATUS, PAGE_TUNE, PAGE_DIAG };

struct App {
    HWND hwnd = nullptr;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    ID2D1StrokeStyle* dash = nullptr;
    IDWriteTextFormat *f_title = nullptr, *f_logo = nullptr, *f_h2 = nullptr, *f_big = nullptr, *f_body = nullptr,
                      *f_small = nullptr, *f_label = nullptr, *f_mono = nullptr, *f_mono_big = nullptr, *f_nav = nullptr,
                      *f_path = nullptr;   // file paths: a Latin font, see make_formats
    float dpi = 96;

    Page page = PAGE_STATUS;
    Config cfg;
    FILETIME ini_time{};
    Trail trail[2];
    LogFacts log;
    std::wstring log_path;
    std::wstring status_note;       // short feedback such as "saved"
    uint64_t note_until = 0;
    bool steamvr = false, registered = false;
    int drag_hand = -1;             // per hand slider being dragged
    bool drag_average = false;      // that slider is the average, else the deadzone
    D2D1_RECT_F drag_track{};
    bool exiting = false, from_steamvr = false;
    NOTIFYICONDATAW nid{};

    // language
    std::vector<LangInfo> langs;    // available, sorted by code
    Lang en, lang;                  // English (for missing strings) and the current one
    std::wstring lang_setting;      // the ini value it was resolved from
    bool rtl = false;               // page laid out right to left
    bool mirror = false;            // the world transform is mirrored right now
    D2D1_RECT_F lang_button{};
    std::wstring key_esc = L"Esc", key_enter = L"Enter", key_tab = L"Tab";   // as the keyboard layout names them
    std::vector<std::wstring> overflow;   // text that did not fit, for --screenshot
};

App g;

// the current language's string, English when it has none
const std::wstring& tr(Str k) { return g.lang.s[k].empty() ? g.en.s[k] : g.lang.s[k]; }

// keep a value or a Latin name in its own left to right run inside right to left text,
// so "0.45", "%LOCALAPPDATA%\..." or "Quest3 (Left)" are not reordered. It uses the
// LRE/PDF embedding marks: DirectWrite on Windows 10/11 ignores the newer LRI/PDI isolates.
// The RLM before each value stops two values joined by ", " from merging into one
// run; none after it, so a unit such as "%" stays on the value's own side.
std::wstring iso(const std::wstring& v) {
    return g.rtl ? std::wstring(L"\x200f\x202a") + v + L"\x202c" : v;
}

// fills {name} placeholders; values are isolated, so they keep their own direction
std::wstring sub(const std::wstring& s, std::initializer_list<std::pair<const wchar_t*, std::wstring>> vals) {
    std::wstring out = s;
    for (auto& [name, v] : vals) {
        std::wstring ph = std::wstring(L"{") + name + L"}", put = iso(v);
        for (size_t p = out.find(ph); p != std::wstring::npos; p = out.find(ph, p + put.size()))
            out.replace(p, ph.size(), put);
    }
    return out;
}

void apply_language(const std::wstring& setting);

std::wstring num(double v, int decimals) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.*f", decimals, v);
    return buf;
}

void note(const std::wstring& s) { g.status_note = s; g.note_until = GetTickCount64() + 2000; }

// opt-in trace for testing: set ANCORA_TRACE=1, lines go to %TEMP%\ancora_trace.log
void trace(const char* f, ...) {
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("ANCORA_TRACE", nullptr, 0) > 0;
    if (!on) return;
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, "ancora_trace.log");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "a") || !fp) return;
    va_list ap;
    va_start(ap, f);
    vfprintf(fp, f, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

// ---- focusable regions -------------------------------------------------------------------
enum Id { ID_NONE, ID_NAV_STATUS, ID_NAV_TUNE, ID_NAV_DIAG, ID_MASTER, ID_CORRECT, ID_GATE, ID_AVERAGE, ID_SLIDER,
          ID_TUNE_HAND, ID_TUNE_START, ID_TUNE_APPLY, ID_TUNE_RESET, ID_TUNE_EDGE, ID_TUNE_EDGE_APPLY,
          ID_TUNE_EDGE_CLEAR, ID_EDGE_ON, ID_TUNE_EDGE_FINISH, ID_COPY, ID_OPEN_CFG, ID_OPEN_LOG, ID_LANG };
struct Region { D2D1_RECT_F r; Id id; int hand; };
std::vector<Region> g_regions;     // rebuilt every frame, in focus order
Id g_focus = ID_NAV_STATUS;
int g_focus_hand = 0;
bool g_focus_visible = false;      // keyboard focus ring only after a key press

bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
bool focused(Id id, int hand = 0) { return g_focus_visible && g_focus == id && g_focus_hand == hand; }
void region(D2D1_RECT_F r, Id id, int hand = 0) { g_regions.push_back({r, id, hand}); }

// ---- drawing helpers -----------------------------------------------------------------
// ui text uses the language's font and direction; mono is for numbers and log
// lines and always reads left to right
IDWriteTextFormat* make_format(float size, DWRITE_FONT_WEIGHT w, bool mono = false, const wchar_t* font = nullptr) {
    IDWriteTextFormat* f = nullptr;
    const wchar_t* locale = lang_code_ok(g.lang.info.code) && g.lang.info.code.rfind(L"qps-", 0) != 0
                                ? g.lang.info.code.c_str() : L"en-us";
    if (!font) font = mono ? L"Consolas" : g.lang.info.font.c_str();
    g.dw->CreateTextFormat(font, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, locale, &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetReadingDirection(g.rtl && !mono ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    return f;
}

bool mono_format(IDWriteTextFormat* f) { return f == g.f_mono || f == g.f_mono_big; }

const D2D1_MATRIX_3X2_F kMirror = {-1, 0, 0, 1, kW, 0};

// draws a layout at layout position x (left edge of a box w wide); in a mirrored
// page the box moves to its mirror image and the text is drawn unmirrored there
void draw_layout(ID2D1RenderTarget* t, IDWriteTextLayout* lay, float x, float y, float w) {
    if (g.mirror) {
        t->SetTransform(D2D1::Matrix3x2F::Identity());
        t->DrawTextLayout(D2D1::Point2F(kW - x - w, y), lay, g.br);
        t->SetTransform(kMirror);
    } else {
        t->DrawTextLayout(D2D1::Point2F(x, y), lay, g.br);
    }
}

void note_overflow(const std::wstring& s, float need, float have) {
    wchar_t buf[48];
    swprintf(buf, 48, L"  (needs %.0f, has %.0f)", need, have);
    if (g.overflow.size() < 200) g.overflow.push_back(s + buf);
}

// one line of text; w is the space it may use: text wider than that shrinks to
// fit, down to 75 %, and what still does not fit is listed for --screenshot
void text(ID2D1RenderTarget* t, const std::wstring& s, IDWriteTextFormat* f, float x, float y, uint32_t c,
          float w = 800, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextLayout* lay = nullptr;
    if (FAILED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, 60, &lay))) return;
    // left to right text in a mirrored page keeps to the mirrored side of its box
    if (g.mirror && mono_format(f) && al != DWRITE_TEXT_ALIGNMENT_CENTER)
        al = al == DWRITE_TEXT_ALIGNMENT_LEADING ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
    lay->SetTextAlignment(al);
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    float dy = 0;
    if (m.widthIncludingTrailingWhitespace > w + 0.5f) {
        float size = f->GetFontSize(), k = std::fmax(0.75f, w / m.widthIncludingTrailingWhitespace);
        lay->SetFontSize(size * k, {0, (UINT32)s.size()});
        dy = size * (1 - k) * 0.6f;   // keep the shrunk line on the same middle
        if (k <= 0.75f) {
            lay->GetMetrics(&m);
            if (m.widthIncludingTrailingWhitespace > w + 0.5f) note_overflow(s, m.widthIncludingTrailingWhitespace, w);
        }
    }
    g.br->SetColor(rgb(c));
    draw_layout(t, lay, x, y + dy, w);
    lay->Release();
}

// wrapped paragraph inside width w and, when given, height h (it shrinks to fit that)
void para(ID2D1RenderTarget* t, const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, uint32_t c,
          float h = 0) {
    IDWriteTextLayout* lay = nullptr;
    if (FAILED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, 400, &lay))) return;
    lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (h > 0) {
        DWRITE_TEXT_METRICS m{};
        float size = f->GetFontSize();
        for (float k = 1.0f; ; k -= 0.05f) {
            lay->GetMetrics(&m);
            if (m.height <= h + 0.5f) break;
            if (k < 0.8f) { note_overflow(s, m.height, h); break; }
            lay->SetFontSize(size * (k - 0.05f), {0, (UINT32)s.size()});
        }
    }
    g.br->SetColor(rgb(c));
    draw_layout(t, lay, x, y, w);
    lay->Release();
}

// stick plots show physical directions, so they never mirror: physical(cx) returns
// where the plot centre goes and drops the mirror until unphysical()
float physical(ID2D1RenderTarget* t, float cx) {
    if (!g.mirror) return cx;
    g.mirror = false;
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    return kW - cx;
}

void unphysical(ID2D1RenderTarget* t) {
    if (!g.rtl) return;
    g.mirror = true;
    t->SetTransform(kMirror);
}

float measure(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* lay = nullptr;
    g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 2000, 60, &lay);
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.width;
}

void fill_round(ID2D1RenderTarget* t, D2D1_RECT_F r, float rad, uint32_t c, float a = 1.0f) {
    g.br->SetColor(rgb(c, a));
    t->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g.br);
}

void stroke_round(ID2D1RenderTarget* t, D2D1_RECT_F r, float rad, uint32_t c, float w = 1, float a = 1.0f) {
    g.br->SetColor(rgb(c, a));
    t->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g.br, w);
}

// white focus ring, the SteamOS style selection cue
void ring(ID2D1RenderTarget* t, D2D1_RECT_F r, float rad) {
    stroke_round(t, D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), rad + 3, kWhite, 2);
}

void circle(ID2D1RenderTarget* t, float x, float y, float r, uint32_t c, float a = 1.0f, bool fill = true, float w = 1,
            ID2D1StrokeStyle* style = nullptr) {
    g.br->SetColor(rgb(c, a));
    D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(x, y), r, r);
    if (fill) t->FillEllipse(e, g.br); else t->DrawEllipse(e, g.br, w, style);
}

void line(ID2D1RenderTarget* t, float x0, float y0, float x1, float y1, uint32_t c, float w = 1, float a = 1.0f) {
    g.br->SetColor(rgb(c, a));
    t->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), g.br, w);
}

// weight thickens the strokes for small sizes such as the 16 px icon
void draw_mark(ID2D1RenderTarget* t, float cx, float cy, float r, float weight = 1.0f) {
    circle(t, cx, cy, r, 0x26303C, 1, false, 1.5f * weight);
    float w = r * 0.11f * weight;
    circle(t, cx, cy - r * 0.52f, r * 0.14f, kBrass, 1, false, w);
    line(t, cx, cy - r * 0.38f, cx, cy + r * 0.55f, kBrass, w);
    line(t, cx - r * 0.3f, cy - r * 0.18f, cx + r * 0.3f, cy - r * 0.18f, kBrass, w);
    ID2D1PathGeometry* path = nullptr;
    g.d2d->CreatePathGeometry(&path);
    ID2D1GeometrySink* sink = nullptr;
    path->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(cx - r * 0.52f, cy + r * 0.12f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx - r * 0.45f, cy + r * 0.62f),
                                                          D2D1::Point2F(cx, cy + r * 0.62f)));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx + r * 0.45f, cy + r * 0.62f),
                                                          D2D1::Point2F(cx + r * 0.52f, cy + r * 0.12f)));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    g.br->SetColor(rgb(kBrass));
    t->DrawGeometry(path, g.br, w);
    sink->Release();
    path->Release();
}

void draw_switch(ID2D1RenderTarget* t, float x, float y, bool on, uint32_t on_color = kOut) {
    D2D1_RECT_F r = {x, y, x + 46, y + 26};
    fill_round(t, r, 13, on ? on_color : kGrid);
    circle(t, on ? x + 33 : x + 13, y + 13, 10, on ? kBg : kDim);
}

// fill: a colour of its own (dark text on it), else green when primary, card when not
void draw_button(ID2D1RenderTarget* t, D2D1_RECT_F r, const std::wstring& s, bool primary, Id id, int hand = 0,
                 uint32_t fill = 0) {
    bool solid = primary || fill;
    fill_round(t, r, 12, fill ? fill : primary ? kOut : kCard);
    text(t, s, g.f_h2, r.left + 12, r.top + (r.bottom - r.top - 24) / 2, solid ? kBg : kText, r.right - r.left - 24,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    if (focused(id, hand)) ring(t, r, 12);
    region(r, id, hand);
}

std::wstring fmt2(const wchar_t* f, double a, double b) {
    wchar_t buf[64];
    swprintf(buf, 64, f, a, b);
    return buf;
}

std::wstring fmt1(const wchar_t* f, double a) {
    wchar_t buf[96];
    swprintf(buf, 96, f, a);
    return buf;
}

// polar target: rings, crosshair, optional deadzone ring
void draw_target(ID2D1RenderTarget* t, float cx, float cy, float R, float dz, bool dashed_dz) {
    for (int k = 1; k <= 3; ++k) circle(t, cx, cy, R * k / 3, kGrid, 1, false, 1);
    line(t, cx - R, cy, cx + R, cy, kGrid);
    line(t, cx, cy - R, cx, cy + R, kGrid);
    if (dz > 0) {
        circle(t, cx, cy, R * dz, kOut, 0.07f);
        circle(t, cx, cy, R * dz, kOut, 0.8f, false, dashed_dz ? 2.0f : 1.0f, dashed_dz ? g.dash : nullptr);
    }
}

// edge stretch outline: how far the stick reaches in each direction
void draw_edge(ID2D1RenderTarget* t, float cx, float cy, float R, const EdgeReach& e, float w, float a) {
    const int n = 192;
    float px = 0, py = 0;
    for (int k = 0; k <= n; ++k) {
        float ang = k * 2 * 3.14159265f / n, x = std::cos(ang), y = std::sin(ang), r = edge_reach(e, x, y);
        float qx = cx + x * r * R, qy = cy - y * r * R;
        if (k) line(t, px, py, qx, qy, kBrass, w, a);
        px = qx; py = qy;
    }
}

// ---- navigation and footer ---------------------------------------------------------------
void nav_icon(ID2D1RenderTarget* t, int which, float x, float y, uint32_t c) {
    if (which == 0) {            // status: ring with a dot
        circle(t, x + 11, y + 11, 8.5f, c, 1, false, 1.8f);
        circle(t, x + 11, y + 11, 3, c);
    } else if (which == 1) {     // tune: two sliders
        line(t, x + 4, y + 7, x + 18, y + 7, c, 1.8f);
        line(t, x + 4, y + 15, x + 18, y + 15, c, 1.8f);
        circle(t, x + 8, y + 7, 2.6f, c);
        circle(t, x + 14, y + 15, 2.6f, c);
    } else {                     // diagnostics: pulse line
        float px[6] = {3, 7, 9, 13, 15, 19}, py[6] = {12, 12, 7, 17, 12, 12};
        for (int i = 0; i < 5; ++i) line(t, x + px[i], y + py[i], x + px[i + 1], y + py[i + 1], c, 1.8f);
    }
}

void draw_nav(ID2D1RenderTarget* t, uint64_t now) {
    g.br->SetColor(rgb(kNav));
    t->FillRectangle(D2D1::RectF(0, 0, kNavW, kH), g.br);
    draw_mark(t, 48, 46, 19);
    text(t, L"ancora", g.f_logo, 78, 30, kText);

    const Str names[3] = {S_nav_status, S_nav_tune, S_nav_diag};
    Id ids[3] = {ID_NAV_STATUS, ID_NAV_TUNE, ID_NAV_DIAG};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = {16, 98 + i * 62.0f, kNavW - 16, 154 + i * 62.0f};
        bool sel = g.page == (Page)i;
        if (sel) fill_round(t, r, 12, kSel);
        nav_icon(t, i, r.left + 16, r.top + 17, sel ? kWhite : kMuted);
        text(t, tr(names[i]), g.f_nav, r.left + 52, r.top + 15, sel ? kWhite : kMuted, r.right - r.left - 64);
        if (focused(ids[i])) ring(t, r, 12);
        region(r, ids[i]);
    }

    // language button: globe and the language's own name, opens the language menu
    D2D1_RECT_F lb = {16, kH - 152, kNavW - 16, kH - 116};
    g.lang_button = lb;
    float gx = lb.left + 27, gy = (lb.top + lb.bottom) / 2;
    circle(t, gx, gy, 8, kDim, 1, false, 1.4f);
    D2D1_ELLIPSE meridian = D2D1::Ellipse(D2D1::Point2F(gx, gy), 3.6f, 8);
    g.br->SetColor(rgb(kDim));
    t->DrawEllipse(meridian, g.br, 1.2f);
    line(t, gx - 8, gy, gx + 8, gy, kDim, 1.2f);
    text(t, g.lang.info.name, g.f_small, lb.left + 46, lb.top + 9, kMuted, lb.right - lb.left - 58);
    if (focused(ID_LANG)) ring(t, lb, 10);
    region(lb, ID_LANG);

    // live summary at the bottom of the rail
    D2D1_RECT_F box = {16, kH - 104, kNavW - 16, kH - 20};
    fill_round(t, box, 12, 0x121821);
    bool live = live_now(now);
    int found = 0;
    for (int i = 0; i < 2; ++i) found += live && connected(g_live.snap.hand[i], now);
    float bw = box.right - box.left - 44;
    circle(t, box.left + 20, box.top + 25, 4, g.steamvr ? kOut : kDim);
    text(t, tr(g.steamvr ? S_rail_steamvr_on : S_rail_steamvr_off), g.f_small, box.left + 32, box.top + 15, kMuted, bw);
    circle(t, box.left + 20, box.top + 57, 4, found == 2 ? kOut : found ? kBrass : kDim);
    text(t, tr(found == 2 ? S_rail_both : found == 1 ? S_rail_one : S_rail_none), g.f_small,
         box.left + 32, box.top + 47, kMuted, bw);
}

// a key's name as the active keyboard layout prints it, from its scan code:
// "Enter" on a US layout, "Eingabe" on a German one, "Entrée" on a French one.
// Layouts return names in capitals ("ESC", "EINGABE"); shown as "Esc", "Eingabe".
std::wstring key_name(UINT scan, const wchar_t* fallback) {
    wchar_t buf[32]{};
    int n = GetKeyNameTextW((LONG)(scan << 16), buf, 32);
    if (n <= 0) return fallback;
    std::wstring s(buf, (size_t)n);
    bool caps = true;
    for (wchar_t c : s) if (IsCharLowerW(c)) caps = false;
    if (caps && s.size() > 1) CharLowerBuffW(&s[1], (DWORD)(s.size() - 1));
    return s;
}

// on start and whenever the keyboard layout changes (WM_INPUTLANGCHANGE)
void read_key_names() {
    g.key_esc = key_name(0x01, L"Esc");
    g.key_tab = key_name(0x0F, L"Tab");
    g.key_enter = key_name(0x1C, L"Enter");
}

void key_hint(ID2D1RenderTarget* t, float& x, float y, const std::wstring& key, const std::wstring& what) {
    // drawn right to left: x is the right edge, moved left past this hint
    float tw = measure(what, g.f_body), kw = measure(key, g.f_label) + 16;
    x -= tw;
    text(t, what, g.f_body, x, y + 4, 0xC6D0DA);
    x -= kw + 10;
    fill_round(t, {x, y + 2, x + kw, y + 26}, 6, kText);
    text(t, key, g.f_label, x, y + 6, kBg, kw, DWRITE_TEXT_ALIGNMENT_CENTER);
    x -= 28;
}

void draw_footer(ID2D1RenderTarget* t) {
    g.br->SetColor(rgb(kNav));
    t->FillRectangle(D2D1::RectF(kNavW, kH - kFootH, kW, kH), g.br);
    float x = kW - 36, y = kH - kFootH + 14;
    key_hint(t, x, y, g.key_esc, tr(S_hint_back));
    bool review = g_tune.stage == TUNE_REVIEW || g_tune.stage == TUNE_EDGE_REVIEW;
    key_hint(t, x, y, g.key_enter, tr(g.page == PAGE_TUNE && review ? S_hint_apply : S_hint_select));
    key_hint(t, x, y, g.key_tab, tr(S_hint_next));
    if (GetTickCount64() < g.note_until)
        text(t, g.status_note, g.f_body, kNavW + 32, y + 4, kOut, std::fmin(400.0f, x - kNavW - 40));
}

// ---- status page ----------------------------------------------------------------------------
struct HandView {
    std::wstring word, sub;
    uint32_t color;
};

HandView describe_hand(int i, uint64_t now) {
    const AncoraState& s = g_live.snap;
    const AncoraHandState& h = s.hand[i];
    bool live = live_now(now);
    float m = std::sqrt(h.raw_x * h.raw_x + h.raw_y * h.raw_y);
    if (!live) return {tr(S_state_no_signal), tr(S_sub_no_signal), kDim};
    if ((h.tracked & 3) != 3 || !h.last_update_ms) return {tr(S_state_not_connected), tr(S_sub_wake), kWarn};
    if (!connected(h, now))
        return {tr(S_state_not_connected), sub(tr(S_sub_silent), {{L"n", num((now - h.last_update_ms) / 1000.0, 0)}}), kWarn};
    if (!s.enable || !h.enable) return {tr(S_state_not_corrected), tr(S_sub_unchanged), kMuted};
    if (h.touch_gate && (h.tracked & 4) && !h.touch)
        return {tr(S_state_anchored), m > 0.02f ? sub(tr(S_sub_drifting), {{L"value", num(m, 2)}}) : tr(S_sub_thumb_off),
                kOut};
    std::wstring pct = sub(tr(S_value_percent), {{L"n", num(h.deadzone * 100, 0)}});
    if (h.deadzone > 0 && m < h.deadzone)
        return {tr(S_state_in_deadzone), sub(tr(S_sub_in_deadzone), {{L"percent", pct}}), kOut};
    return {tr(S_state_following), h.deadzone > 0 ? sub(tr(S_sub_past_deadzone), {{L"percent", pct}}) : tr(S_sub_as_is),
            kBrass};
}

void draw_trail(ID2D1RenderTarget* t, int i, float cx, float cy, float R, uint64_t now) {
    const Trail& tr = g.trail[i];
    for (int n = 1; n < tr.count; ++n) {
        const TrailPoint& a = tr.p[(tr.head - n + kTrail) % kTrail];
        const TrailPoint& b = tr.p[(tr.head - n - 1 + kTrail) % kTrail];
        if (now - b.t > kTrailMs) break;
        float alpha = 0.85f * (1.0f - (float)(now - a.t) / kTrailMs);
        line(t, cx + b.x * R, cy - b.y * R, cx + a.x * R, cy - a.y * R, kRaw, 2.2f, alpha);
    }
}

// one settings row: label, slider (fraction 0..1) and value
// label_w: the widest label in the group, so the tracks line up
void draw_slider(ID2D1RenderTarget* t, D2D1_RECT_F r, const std::wstring& label, const std::wstring& val, bool active,
                 float fraction, Id id, int hand, float label_w) {
    float x0 = r.left + std::fmax(110.0f, std::fmin(label_w + 30, 190.0f)), x1 = r.right - 88;
    float y = (r.top + r.bottom) / 2, ty = y - 11;   // centred in the row, whatever its height
    text(t, label, g.f_body, r.left + 14, ty, kText, x0 - r.left - 26);
    text(t, val, g.f_body, r.right - 80, ty, active ? kText : kMuted, 66, DWRITE_TEXT_ALIGNMENT_TRAILING);
    line(t, x0, y, x1, y, kGrid, 4);
    float kx = x0 + (x1 - x0) * fraction;
    line(t, x0, y, kx, y, kOut, 4);
    circle(t, kx, y, 9, kText);
    if (focused(id, hand)) ring(t, r, 12);
    region({x0 - 12, r.top, x1 + 12, r.bottom}, id, hand);
}

void draw_hand_card(ID2D1RenderTarget* t, int i, uint64_t now) {
    float x0 = 292 + i * 486.0f;
    D2D1_RECT_F c = {x0, 84, x0 + 470, kH - kFootH - 24};
    fill_round(t, c, 18, kCard);
    const AncoraHandState& h = g_live.snap.hand[i];
    const HandConfig& hc = g.cfg.hand[i];
    bool live = live_now(now), tracked = live && connected(h, now);

    text(t, tr(i ? S_hand_right : S_hand_left), g.f_h2, x0 + 22, c.top + 18, kText, 470 - 44 - 46 - 16);
    D2D1_RECT_F sw = {x0 + 470 - 22 - 46, c.top + 18, x0 + 470 - 22, c.top + 44};
    draw_switch(t, sw.left, sw.top, hc.enable);
    if (focused(ID_CORRECT, i)) ring(t, sw, 13);
    region(sw, ID_CORRECT, i);

    // stick
    float R = 82, cx = physical(t, x0 + 22 + R), cy = c.top + 62 + R;
    draw_target(t, cx, cy, R, (live && h.enable && g_live.snap.enable) ? h.deadzone : 0, false);
    if (hc.edge.on && hc.edge_on && hc.enable && g.cfg.enable) draw_edge(t, cx, cy, R, hc.edge, 1.2f, 0.7f);
    draw_trail(t, i, cx, cy, R, now);
    if (tracked) {
        circle(t, cx + h.raw_x * R, cy - h.raw_y * R, 7, kRaw, 1, false, 2);
        circle(t, cx + h.out_x * R, cy - h.out_y * R, 15, kOut, 0.18f);
        circle(t, cx + h.out_x * R, cy - h.out_y * R, 8, kOut);
    }
    unphysical(t);

    HandView v = describe_hand(i, now);
    text(t, v.word, g.f_big, x0 + 210, c.top + 92, v.color, 240);
    para(t, v.sub, g.f_body, x0 + 210, c.top + 134, 240, kMuted, 100);

    // readouts
    float ry = c.top + 240, bw = (470 - 44 - 16) / 3.0f;
    const Str lab[3] = {S_read_reports, S_read_games, S_read_thumb};
    for (int k = 0; k < 3; ++k) {
        D2D1_RECT_F b = {x0 + 22 + k * (bw + 8), ry, x0 + 22 + k * (bw + 8) + bw, ry + 50};
        fill_round(t, b, 10, kWell);
        text(t, tr(lab[k]), g.f_small, b.left + 12, b.top + 5, kDim, bw - 24);
        if (!tracked) { text(t, L"--", g.f_mono, b.left + 12, b.top + 24, kDim, bw - 24); continue; }
        if (k == 0) text(t, fmt2(L"%+.2f %+.2f", h.raw_x, h.raw_y), g.f_mono, b.left + 12, b.top + 24, kRawText, bw - 24);
        if (k == 1) text(t, fmt2(L"%+.2f %+.2f", h.out_x, h.out_y), g.f_mono, b.left + 12, b.top + 24, kOut, bw - 24);
        if (k == 2) text(t, tr(thumb_label(h)), g.f_body, b.left + 12, b.top + 23, thumb_color(h), bw - 24);
    }

    // per hand settings: four rows
    const float rh = 46;
    D2D1_RECT_F well = {x0 + 22, ry + 60, x0 + 470 - 22, ry + 60 + 4 * rh};
    fill_round(t, well, 12, kWell);
    auto row = [&](int n) { return D2D1_RECT_F{well.left, well.top + n * rh, well.right, well.top + (n + 1) * rh}; };
    D2D1_RECT_F gate = row(0);
    text(t, tr(S_set_gate), g.f_body, gate.left + 14, gate.top + rh / 2 - 11, kText, gate.right - gate.left - 90);
    draw_switch(t, gate.right - 14 - 46, gate.top + rh / 2 - 13, hc.touch_gate);
    if (focused(ID_GATE, i)) ring(t, gate, 12);
    region(gate, ID_GATE, i);
    for (int n = 1; n < 4; ++n) line(t, well.left, well.top + n * rh, well.right, well.top + n * rh, kLine);

    float label_w = std::fmax(measure(tr(S_set_smoothing), g.f_body), measure(tr(S_set_deadzone), g.f_body));
    draw_slider(t, row(1), tr(S_set_smoothing),
                hc.average_ms > 0 ? sub(tr(S_value_ms), {{L"n", std::to_wstring(hc.average_ms)}}) : tr(S_value_off),
                hc.average_ms > 0, hc.average_ms / (float)kMaxAverageMs, ID_AVERAGE, i, label_w);
    draw_slider(t, row(2), tr(S_set_deadzone),
                // the whole "31%" as one left to right run, so the sign keeps to its number
                hc.deadzone > 0 ? iso(sub(tr(S_value_percent), {{L"n", num(hc.deadzone * 100, 0)}})) : tr(S_value_off),
                hc.deadzone > 0, hc.deadzone / 0.95f, ID_SLIDER, i, label_w);

    // edge stretch: a switch once measured, else a pointer to the Tune page
    D2D1_RECT_F edge = row(3);
    text(t, tr(S_set_edge), g.f_body, edge.left + 14, edge.top + rh / 2 - 11, kText, edge.right - edge.left - 200);
    if (hc.edge.on) {
        draw_switch(t, edge.right - 14 - 46, edge.top + rh / 2 - 13, hc.edge_on, kBrass);
    } else {
        text(t, tr(S_edge_not_measured), g.f_body, edge.right - 194, edge.top + rh / 2 - 11, kMuted, 180,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (focused(ID_EDGE_ON, i)) ring(t, edge, 12);
    region(edge, ID_EDGE_ON, i);
}

void draw_status(ID2D1RenderTarget* t, uint64_t now) {
    text(t, tr(S_nav_status), g.f_title, 292, 24, kText, 400);
    // master switch pill
    std::wstring label = tr(g.cfg.enable ? S_master_on : S_master_off);
    float w = measure(label, g.f_h2) + 46 + 44;
    D2D1_RECT_F r = {kW - 32 - w, 22, kW - 32, 66};
    fill_round(t, r, 22, kCard);
    text(t, label, g.f_h2, r.left + 18, r.top + 10, kText);
    draw_switch(t, r.right - 12 - 46, r.top + 9, g.cfg.enable);
    if (focused(ID_MASTER)) ring(t, r, 22);
    region(r, ID_MASTER);
    for (int i = 0; i < 2; ++i) draw_hand_card(t, i, now);
}

// ---- tune page ---------------------------------------------------------------------------------
void draw_step(ID2D1RenderTarget* t, D2D1_RECT_F r, int n, int state, const std::wstring& title, const std::wstring& sub,
               float progress) {
    // state: 0 waiting, 1 active, 2 done
    fill_round(t, r, 14, state == 1 ? kSel : kCard);
    float cx = r.left + 30, cy = r.top + 28;
    if (state == 2) {
        circle(t, cx, cy, 14, kOkWell);
        float kx = physical(t, cx);   // a check mark reads the same in every direction
        line(t, kx - 6, cy, kx - 2, cy + 4, kOut, 2.2f);
        line(t, kx - 2, cy + 4, kx + 6, cy - 4, kOut, 2.2f);
        unphysical(t);
    } else if (state == 1) {
        circle(t, cx, cy, 14, kOut);
        text(t, std::to_wstring(n), g.f_label, cx - 14, cy - 9, kBg, 28, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        circle(t, cx, cy, 13, 0x3A4552, 1, false, 1.5f);
        text(t, std::to_wstring(n), g.f_label, cx - 14, cy - 9, kDim, 28, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    float tw = r.right - r.left - 70 - (state == 1 && progress >= 0 ? 160 : 0);
    text(t, title, g.f_h2, r.left + 58, r.top + (sub.empty() ? 16 : 8), state == 0 ? kMuted : kText, tw);
    if (!sub.empty()) text(t, sub, g.f_small, r.left + 58, r.top + 32, kMuted, tw);
    if (state == 1 && progress >= 0) {
        D2D1_RECT_F bar = {r.right - 150, r.top + 25, r.right - 20, r.top + 31};
        fill_round(t, bar, 3, kBg);
        fill_round(t, {bar.left, bar.top, bar.left + (bar.right - bar.left) * progress, bar.bottom}, 3, kOut);
    }
}

void draw_tune(ID2D1RenderTarget* t, uint64_t now) {
    text(t, tr(S_nav_tune), g.f_title, 292, 24, kText, 400);
    // hand selector
    D2D1_RECT_F seg = {kW - 32 - 320, 22, kW - 32, 66};
    fill_round(t, seg, 12, kCard);
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F b = {seg.left + 4 + i * 156.0f, seg.top + 4, seg.left + 160 + i * 156.0f, seg.bottom - 4};
        bool sel = g_tune.hand == i;
        if (sel) fill_round(t, b, 9, kGrid);
        text(t, tr(i ? S_hand_right : S_hand_left), g.f_body, b.left + 6, b.top + 9, sel ? kWhite : kMuted,
             b.right - b.left - 12, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (focused(ID_TUNE_HAND, i)) ring(t, b, 9);
        region(b, ID_TUNE_HAND, i);
    }

    // measurement target
    D2D1_RECT_F left = {292, 84, 672, kH - kFootH - 24};
    fill_round(t, left, 18, kCard);
    float R = 150, lcx = (left.left + left.right) / 2, cy = (left.top + left.bottom - 40) / 2;
    int st = g_tune.stage;
    bool review = st == TUNE_REVIEW, edge_review = st == TUNE_EDGE_REVIEW;
    bool edge_mode = st == TUNE_EDGE || edge_review;
    const HandConfig& hc = g.cfg.hand[g_tune.hand];
    float cx = physical(t, lcx);
    draw_target(t, cx, cy, R, review ? g_tune.result.suggested : 0, true);
    if (edge_review) {
        draw_edge(t, cx, cy, R, g_tune.edge_result, 2.2f, 1.0f);
    } else if (st == TUNE_EDGE) {
        // every direction: brass once settled, red while it still needs a pass, a dim
        // mark on the rim where the stick has not been yet
        for (int b = 0; b < kEdgeBins; ++b) {
            float r = g_tune.edge.bin[b], a = b * 2 * 3.14159265f / kEdgeBins, c = std::cos(a), s = std::sin(a);
            if (r <= 0) circle(t, cx + c * R, cy - s * R, 2.5f, kDim, 0.8f);
            else circle(t, cx + c * r * R, cy - s * r * R, 3.5f, g_tune.edge.settled(b) ? kBrass : kRaw);
        }
    } else if (st == TUNE_IDLE && hc.edge.on) {
        draw_edge(t, cx, cy, R, hc.edge, 1.2f, 0.6f);
    }
    if (!edge_mode)
        for (const Point& p : g_tune.pts) circle(t, cx + p.x * R, cy - p.y * R, 3, kRaw, 0.8f);
    const AncoraHandState& h = g_live.snap.hand[g_tune.hand];
    if (live_now(now) && connected(h, now) && !review && !edge_review)
        circle(t, cx + h.raw_x * R, cy - h.raw_y * R, 7, kRaw, 1, false, 2);
    unphysical(t);
    if (review && g_tune.result.suggested > 0)
        text(t, sub(tr(S_tune_suggested), {{L"value", num(g_tune.result.suggested, 2)}}), g.f_small, lcx - 100,
             cy + R * g_tune.result.suggested + 6, kOut, 200, DWRITE_TEXT_ALIGNMENT_CENTER);
    // legend, centred under the plot
    float ly = left.bottom - 34;
    if (edge_review) {
        float w = std::fmin(measure(tr(S_legend_reach), g.f_small), left.right - left.left - 80);
        float lx = lcx - (22 + w) / 2;
        line(t, lx, ly + 9, lx + 14, ly + 9, kBrass, 2);
        text(t, tr(S_legend_reach), g.f_small, lx + 22, ly, kMuted, w + 1);
    } else if (edge_mode) {
        float w1 = measure(tr(S_legend_edge_done), g.f_small), w2 = measure(tr(S_legend_edge_more), g.f_small);
        float avail = left.right - left.left - 40 - 12 - 12 - 24;
        float k = w1 + w2 > avail ? avail / (w1 + w2) : 1.0f;
        w1 *= k; w2 *= k;
        float lx = lcx - (12 + w1 + 24 + 12 + w2) / 2;
        circle(t, lx + 4.5f, ly + 9, 4.5f, kBrass);
        text(t, tr(S_legend_edge_done), g.f_small, lx + 12, ly, kMuted, w1 + 1);
        lx += 12 + w1 + 24;
        circle(t, lx + 4.5f, ly + 9, 4.5f, kRaw);
        text(t, tr(S_legend_edge_more), g.f_small, lx + 12, ly, kMuted, w2 + 1);
    } else {
        float w1 = measure(tr(S_legend_rests), g.f_small), w2 = measure(tr(S_legend_suggested), g.f_small);
        float avail = left.right - left.left - 40 - 12 - 22 - 24;
        float k = w1 + w2 > avail ? avail / (w1 + w2) : 1.0f;
        w1 *= k; w2 *= k;
        float lx = lcx - (12 + w1 + 24 + 22 + w2) / 2;
        circle(t, lx + 4.5f, ly + 9, 4.5f, kRaw);
        text(t, tr(S_legend_rests), g.f_small, lx + 12, ly, kMuted, w1 + 1);
        lx += 12 + w1 + 24;
        line(t, lx, ly + 9, lx + 14, ly + 9, kOut, 2);
        text(t, tr(S_legend_suggested), g.f_small, lx + 22, ly, kMuted, w2 + 1);
    }

    // steps
    float x0 = 692, x1 = kW - 32, y = 84;
    static const Str dir_names[8] = {S_dir_right, S_dir_forward_right, S_dir_forward, S_dir_forward_left,
                                     S_dir_left, S_dir_back_left, S_dir_back, S_dir_back_right};
    if (edge_mode) {
        const EdgeMeasure& em = g_tune.edge;
        std::wstring total = std::to_wstring(kEdgeBins);
        int done = em.settled_count();
        draw_step(t, {x0, y, x1, y + 56}, 1, edge_review ? 2 : 1, tr(S_step_edge),
                  edge_review ? sub(tr(S_step_edge_done), {{L"total", total}})
                              : sub(tr(S_step_edge_active), {{L"n", std::to_wstring(done)}, {L"total", total}}),
                  edge_review ? -1 : done / (float)kEdgeBins);
        y += 64;
        draw_step(t, {x0, y, x1, y + 56}, 2, edge_review ? 1 : 0, tr(S_step_review), L"", -1);
        y += 72;
    } else {
        float rest_p = st == TUNE_REST ? std::fmin(1.0f, (now - g_tune.t0) / (float)kRestMs) : -1;
        draw_step(t, {x0, y, x1, y + 56}, 1, st == TUNE_REST ? 1 : st > TUNE_REST ? 2 : 0, tr(S_step_rest),
                  st == TUNE_REST ? tr(S_step_rest_active) :
                  st > TUNE_REST ? sub(tr(S_step_rest_done), {{L"n", std::to_wstring(kRestMs / 1000)}}) : L"", rest_p);
        y += 64;
        std::wstring n = std::to_wstring(g_tune.flicks), total = std::to_wstring(kFlicks);
        draw_step(t, {x0, y, x1, y + 56}, 2, st == TUNE_FLICK ? 1 : st > TUNE_FLICK ? 2 : 0, tr(S_step_flick),
                  st == TUNE_FLICK ? sub(tr(g_tune.armed && g_tune.release_t ? S_step_flick_settle
                                            : g_tune.armed ? S_step_flick_release : S_step_flick_push),
                                         {{L"n", n}, {L"total", total}}) :
                  st > TUNE_FLICK ? sub(tr(S_step_flick_done), {{L"total", total}}) : L"",
                  st == TUNE_FLICK ? g_tune.flicks / (float)kFlicks : -1);
        y += 64;
        draw_step(t, {x0, y, x1, y + 56}, 3, review ? 1 : 0, tr(S_step_review), L"", -1);
        y += 72;
    }

    // results and findings
    int short_k = edge_shortest(g_tune.edge_result);
    float short_r = g_tune.edge_result.r[short_k];
    Str short_dir = dir_names[(int)std::lround(short_k * (360.0 / kEdgeSectors) / 45.0) % 8];
    if (review) {
        const TuneResult& r = g_tune.result;
        D2D1_RECT_F box = {x0, y, x1, y + 168};
        fill_round(t, box, 14, kCard);
        float cw = (x1 - x0 - 32) / 3;
        text(t, fmt1(L"%.2f", r.peak), g.f_mono_big, x0 + 16, y + 12, kRawText, cw - 8);
        text(t, tr(S_res_peak), g.f_small, x0 + 16, y + 46, kMuted, cw - 8);
        text(t, fmt1(L"%.2f", r.typical), g.f_mono_big, x0 + 16 + cw, y + 12, kText, cw - 8);
        text(t, tr(S_res_typical), g.f_small, x0 + 16 + cw, y + 46, kMuted, cw - 8);
        text(t, tr(r.direction), g.f_big, x0 + 16 + 2 * cw, y + 12, kText, cw);
        text(t, tr(S_res_direction), g.f_small, x0 + 16 + 2 * cw, y + 46, kMuted, cw);
        float fy = y + 76;
        if (r.peak < 0.03f) {
            text(t, L"✓", g.f_h2, x0 + 16, fy, kOut, 20);
            para(t, tr(S_find_none), g.f_body, x0 + 40, fy + 2, x1 - x0 - 56, kText, 84);
        } else {
            text(t, r.gate_ok ? L"✓" : L"!", g.f_h2, x0 + 16, fy, r.gate_ok ? kOut : kBrass, 20);
            para(t, tr(r.gate_ok ? S_find_gate_ok : r.has_sensor ? S_find_gate_fail : S_find_no_sensor),
                 g.f_body, x0 + 40, fy + 2, x1 - x0 - 56, kText, 42);
            text(t, L"!", g.f_h2, x0 + 16, fy + 46, kBrass, 20);
            para(t, sub(tr(S_find_creep), {{L"value", num(r.suggested, 2)}}), g.f_body, x0 + 40, fy + 48,
                 x1 - x0 - 56, kText, 42);
        }
    } else if (edge_review) {
        D2D1_RECT_F box = {x0, y, x1, y + 168};
        fill_round(t, box, 14, kCard);
        float cw = (x1 - x0 - 32) / 3;
        text(t, fmt1(L"%.2f", short_r), g.f_mono_big, x0 + 16, y + 12, short_r < kEdgeFine ? kBrass : kOut, cw - 8);
        text(t, tr(S_res_reach), g.f_small, x0 + 16, y + 46, kMuted, cw - 8);
        text(t, tr(short_dir), g.f_big, x0 + 16 + cw, y + 12, kText, 2 * cw);
        text(t, tr(S_res_direction), g.f_small, x0 + 16 + cw, y + 46, kMuted, 2 * cw);
        float fy = y + 76;
        bool fine = short_r >= kEdgeFine;
        text(t, fine ? L"✓" : L"!", g.f_h2, x0 + 16, fy, fine ? kOut : kBrass, 20);
        para(t, fine ? tr(S_find_edge_ok)
                     : sub(tr(S_find_edge_short), {{L"value", num(short_r, 2)}, {L"direction", tr(short_dir)}}),
             g.f_body, x0 + 40, fy + 2, x1 - x0 - 56, kText, 84);
    } else if (st == TUNE_EDGE) {
        para(t, tr(S_edge_gap_hint), g.f_body, x0 + 4, y, x1 - x0 - 8, kMuted, 100);
    } else if (st == TUNE_IDLE) {
        para(t, tr(S_tune_intro), g.f_body, x0 + 4, y, x1 - x0 - 8, kMuted, 84);
        para(t, tr(S_edge_intro), g.f_body, x0 + 4, y + 80, x1 - x0 - 8, kMuted, 100);
    }

    // actions
    // buttons right to left in the steps column; long labels shrink to keep them inside it
    struct Btn { std::wstring s; bool primary; Id id; uint32_t fill = 0; };   // edge actions are brass, like the edge outline
    std::vector<Btn> btns;
    bool right = g_tune.hand != 0;
    if (st == TUNE_IDLE) {
        btns.push_back({tr(S_btn_start), true, ID_TUNE_START});
        btns.push_back({tr(S_btn_edge), false, ID_TUNE_EDGE, kBrass});
        if (hc.edge.on) btns.push_back({tr(right ? S_btn_edge_clear_right : S_btn_edge_clear_left), false, ID_TUNE_EDGE_CLEAR});
    } else if (review && g_tune.result.peak >= 0.03f) {
        btns.push_back({sub(tr(right ? S_btn_apply_right : S_btn_apply_left), {{L"value", num(g_tune.result.suggested, 2)}}),
                        true, ID_TUNE_APPLY});
    } else if (st == TUNE_EDGE && g_tune.edge.can_finish()) {
        btns.push_back({tr(S_btn_edge_finish), false, ID_TUNE_EDGE_FINISH, kBrass});
    } else if (edge_review && short_r < kEdgeFine) {
        btns.push_back({tr(right ? S_btn_edge_apply_right : S_btn_edge_apply_left), false, ID_TUNE_EDGE_APPLY, kBrass});
    }
    if (st != TUNE_IDLE) btns.push_back({tr(S_btn_restart), false, ID_TUNE_RESET});
    float need = 0;
    for (auto& b : btns) need += measure(b.s, g.f_h2) + 44;
    float room = x1 - x0 - 10 * (btns.size() - 1.0f), fit = need > room ? room / need : 1.0f;
    float by = kH - kFootH - 24 - 48, bx = x1;
    for (auto& b : btns) {
        float w = (measure(b.s, g.f_h2) + 44) * fit;
        draw_button(t, {bx - w, by, bx, by + 48}, b.s, b.primary, b.id, 0, b.fill);
        bx -= w + 10;
    }
}

// ---- diagnostics page ---------------------------------------------------------------------
void draw_diag(ID2D1RenderTarget* t, uint64_t now) {
    text(t, tr(S_nav_diag), g.f_title, 292, 24, kText, 400);
    float x0 = 292, x1 = kW - 32, y = 84, tw = (x1 - x0 - 24) / 3;
    // version, so a copied report or a screenshot says which build it came from
    text(t, iso(std::wstring(L"ancora ") + ANCORA_VERSION), g.f_body, x1 - 300, 36, kMuted, 300,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    const LogFacts& lf = g.log;
    // the live shared state first: the log facts come from the end of SteamVR's log,
    // and a long session (Steam Link logs every few seconds) pushes the driver's
    // load lines out of the part read, while the driver runs fine
    bool live = live_now(now);
    int generation = live && g_live.snap.generation > 0 ? (int)g_live.snap.generation : lf.generation;
    bool same_gen = lf.generation == generation;   // the log's load line is this logic's
    bool driver_running = live || lf.tables > 0;
    struct Tile { Str head; std::wstring main, sub; IDWriteTextFormat* sub_font; };
    Tile tiles[3] = {
        {S_tile_driver, tr(g.registered ? (driver_running ? S_drv_loaded : S_drv_registered) : S_drv_missing),
         lf.tables > 0 ? sub(tr(S_drv_patched), {{L"n", std::to_wstring(lf.tables)}})
                       : driver_running ? std::wstring() : tr(S_drv_not_seen), g.f_small},
        {S_tile_logic, generation > 0 ? sub(tr(S_logic_gen), {{L"n", std::to_wstring(generation)}}) : tr(S_logic_none),
         generation > 0 && same_gen ? sub(tr(S_logic_loaded), {{L"time", lf.gen_time}, {L"n", std::to_wstring(lf.replayed)}})
                                    : std::wstring(), g.f_small},
        {S_tile_config, lf.config_time.empty() ? tr(S_cfg_pending) : sub(tr(S_cfg_applied), {{L"time", lf.config_time}}),
         iso(L"%LOCALAPPDATA%\\ancora\\ancora.ini"), g.f_path}};
    for (int k = 0; k < 3; ++k) {
        D2D1_RECT_F b = {x0 + k * (tw + 12), y, x0 + k * (tw + 12) + tw, y + 80};
        fill_round(t, b, 14, kCard);
        text(t, tr(tiles[k].head), g.f_small, b.left + 16, b.top + 12, kMuted, tw - 32);
        text(t, tiles[k].main, g.f_h2, b.left + 16, b.top + 30, kText, tw - 32);
        text(t, tiles[k].sub, tiles[k].sub_font, b.left + 16, b.top + 56, kMuted, tw - 32);
    }
    y += 96;

    // controller table
    D2D1_RECT_F tb = {x0, y, x1, y + 136};
    fill_round(t, tb, 14, kCard);
    float cols[7] = {x0 + 16, x0 + 260, x0 + 400, x0 + 510, x0 + 660, x1 - 110, x1 - 16};
    const Str heads[6] = {S_col_controller, S_col_inputs, S_col_thumb, S_read_reports, S_read_games, S_col_updates};
    for (int k = 0; k < 5; ++k) text(t, tr(heads[k]), g.f_small, cols[k], y + 12, kMuted, cols[k + 1] - cols[k] - 12);
    text(t, tr(heads[5]), g.f_small, cols[5] - 20, y + 12, kMuted, 114, DWRITE_TEXT_ALIGNMENT_TRAILING);
    for (int i = 0; i < 2; ++i) {
        float ry = y + 40 + i * 46;
        line(t, x0, ry, x1, ry, kLine);
        const AncoraHandState& h = g_live.snap.hand[i];
        bool tracked = live && connected(h, now);
        text(t, live && h.model[0] ? iso(widen(h.model)) : tr(i ? S_diag_right_untracked : S_diag_left_untracked),
             g.f_body, cols[0], ry + 13, live && h.model[0] ? kText : kDim, cols[1] - cols[0] - 12);
        bool silent = live && (h.tracked & 3) == 3 && !tracked;
        uint32_t ic = tracked ? kOut : silent ? kWarn : kDim;
        if (silent) {
            text(t, tr(S_inputs_silent), g.f_body, cols[1], ry + 13, ic, cols[2] - cols[1] - 12);
        } else {
            std::wstring inputs = live ? std::wstring(h.tracked & 1 ? L"x " : L"") + (h.tracked & 2 ? L"y " : L"") +
                                             (h.tracked & 4 ? L"touch" : L"")
                                       : L"--";
            text(t, inputs, g.f_mono, cols[1], ry + 14, ic, cols[2] - cols[1] - 12);
        }
        if (!tracked) text(t, L"--", g.f_mono, cols[2], ry + 14, kMuted, cols[3] - cols[2] - 12);
        else text(t, tr(thumb_label(h)), g.f_body, cols[2], ry + 13, thumb_color(h), cols[3] - cols[2] - 12);
        text(t, tracked ? fmt2(L"%+.2f %+.2f", h.raw_x, h.raw_y) : L"--", g.f_mono, cols[3], ry + 14, kRawText,
             cols[4] - cols[3] - 12);
        text(t, tracked ? fmt2(L"%+.2f %+.2f", h.out_x, h.out_y) : L"--", g.f_mono, cols[4], ry + 14, kOut,
             cols[5] - cols[4] - 12);
        if (live && g_live.rate[i] < 1) text(t, tr(S_rate_idle), g.f_body, cols[5] - 20, ry + 13, kText, 114,
                                             DWRITE_TEXT_ALIGNMENT_TRAILING);
        else text(t, live ? fmt1(L"%.0f/s", g_live.rate[i]) : L"--", g.f_mono, cols[5] - 20, ry + 14, kText, 114,
                  DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    y += 152;

    // log tail: SteamVR's own lines, left to right in every language
    D2D1_RECT_F lb = {x0, y, x1, kH - kFootH - 24 - 64};
    fill_round(t, lb, 14, kNav);
    float hw = lb.right - lb.left - 32;
    int missing = g.lang.missing();
    if (missing && g.lang.info.code != L"en") {
        // untranslated strings are shown in English; say so where people look for problems
        std::wstring m = sub(tr(S_lang_missing), {{L"name", g.lang.info.name}, {L"n", std::to_wstring(missing)}});
        text(t, m, g.f_small, lb.left + 16, lb.top + 10, kWarn, hw, DWRITE_TEXT_ALIGNMENT_TRAILING);
        hw = hw - std::fmin(hw * 0.6f, measure(m, g.f_small)) - 16;
    }
    text(t, tr(S_log_title), g.f_small, lb.left + 16, lb.top + 10, kMuted, hw);
    size_t n = lf.lines.size(), rows = (size_t)((lb.bottom - lb.top - 36) / 19), first = n > rows ? n - rows : 0;
    t->PushAxisAlignedClip(D2D1::RectF(lb.left + 8, lb.top + 30, lb.right - 8, lb.bottom - 6), D2D1_ANTIALIAS_MODE_ALIASED);
    float ly = lb.top + 32;
    for (size_t k = first; k < n; ++k, ly += 19) text(t, lf.lines[k], g.f_mono, lb.left + 16, ly, kMuted, 3000);
    if (!n) text(t, tr(S_log_empty), g.f_body, lb.left + 16, ly, kDim, lb.right - lb.left - 32);
    t->PopAxisAlignedClip();

    // actions
    float by = kH - kFootH - 24 - 48, bx = x0;
    auto button_left = [&](const std::wstring& s, Id id) {
        float w = measure(s, g.f_h2) + 44;
        draw_button(t, {bx, by, bx + w, by + 48}, s, false, id);
        bx += w + 10;
    };
    button_left(tr(S_btn_copy), ID_COPY);
    button_left(tr(S_btn_open_cfg), ID_OPEN_CFG);
    button_left(tr(S_btn_open_log), ID_OPEN_LOG);
}

void draw(ID2D1RenderTarget* t) {
    uint64_t now = g_fake ? g_live.snap.tick_ms : GetTickCount64();
    g_regions.clear();
    g.overflow.clear();
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    t->Clear(rgb(kBg));
    g.mirror = g.rtl;
    if (g.rtl) t->SetTransform(kMirror);
    draw_nav(t, now);
    if (g.page == PAGE_STATUS) draw_status(t, now);
    if (g.page == PAGE_TUNE) draw_tune(t, now);
    if (g.page == PAGE_DIAG) draw_diag(t, now);
    draw_footer(t);
    g.mirror = false;
    t->SetTransform(D2D1::Matrix3x2F::Identity());
}

// ---- data refresh ------------------------------------------------------------------------------
void push_trail(int i, float x, float y, uint64_t now) {
    Trail& tr = g.trail[i];
    const TrailPoint& last = tr.p[(tr.head - 1 + kTrail) % kTrail];
    if (tr.count && last.x == x && last.y == y) return;
    tr.p[tr.head] = {x, y, now};
    tr.head = (tr.head + 1) % kTrail;
    if (tr.count < kTrail) ++tr.count;
}

// returns true when something visible changed
bool refresh_fast() {
    uint64_t now = GetTickCount64();
    uint32_t before = g_live.snap.seq;
    live_poll();
    bool changed = g_live.snap.seq != before;
    if (g_live.valid)
        for (int i = 0; i < 2; ++i) push_trail(i, g_live.snap.hand[i].raw_x, g_live.snap.hand[i].raw_y, g_live.snap.tick_ms);
    if (g.page == PAGE_TUNE && tune_sample(now)) changed = true;
    if (g.page == PAGE_TUNE && g_tune.stage == TUNE_REST) changed = true;   // progress bar
    // trails keep fading for a while after the last move
    if (g.page == PAGE_STATUS)
        for (int i = 0; i < 2; ++i) {
            const Trail& tr = g.trail[i];
            if (tr.count && now - tr.p[(tr.head - 1 + kTrail) % kTrail].t < kTrailMs + 100) changed = true;
        }
    return changed || g.drag_hand >= 0 || now < g.note_until + 100;
}

// "Thu Oct 29 2026 09:58:12.345 [Info] - ancora: ancora: msg" -> facts and "09:58:12  msg"
void refresh_log() {
    if (g.log_path.empty()) g.log_path = steamvr_log_path();   // SteamVR may be set up later
    std::string tail = read_file_tail(g.log_path, 64 * 1024);
    LogFacts f;
    size_t pos = 0;
    while (pos < tail.size()) {
        size_t e = tail.find('\n', pos);
        if (e == std::string::npos) e = tail.size();
        std::string ln = tail.substr(pos, e - pos);
        pos = e + 1;
        size_t k = ln.rfind("ancora: ");
        if (k == std::string::npos) continue;
        std::string time = ln.size() > 24 ? ln.substr(16, 8) : "";
        std::string msg = ln.substr(k + 8);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
        int a = 0, b = 0;
        if (sscanf_s(msg.c_str(), "patched %d", &a) == 1) f.tables = a;
        if (sscanf_s(msg.c_str(), "logic loaded, generation %d, replayed %d", &a, &b) == 2) {
            f.generation = a; f.replayed = b; f.gen_time = widen(time);
        }
        if (msg.rfind("config enable", 0) == 0) f.config_time = widen(time);
        f.lines.push_back(widen(time + "  " + msg));
    }
    g.log = std::move(f);
}

void refresh_slow() {
    g.steamvr = process_running(L"vrserver.exe");
    g.registered = driver_registered();
    FILETIME ft = file_time(g_ini);
    if (CompareFileTime(&ft, &g.ini_time) != 0 && g.drag_hand < 0) {
        g.ini_time = ft;
        g.cfg = read_config(g_ini);   // picks up hand edits
        if (g.cfg.language != g.lang_setting && g.dw) apply_language(g.cfg.language);
    }
    refresh_log();
    uint64_t now = GetTickCount64();
    if (g_live.rate_tick) {
        float dt = (now - g_live.rate_tick) / 1000.0f;
        for (int i = 0; i < 2; ++i) {
            uint32_t u = g_live.snap.hand[i].updates;
            g_live.rate[i] = dt > 0 ? (u - g_live.last_updates[i]) / dt : 0;
            g_live.last_updates[i] = u;
        }
    }
    g_live.rate_tick = now;
    if (g.from_steamvr && !g.steamvr) PostMessageW(g.hwnd, WM_CLOSE, 1, 0);   // SteamVR quit
}

// ---- actions ---------------------------------------------------------------------------------------
void apply_language(const std::wstring& setting);
void language_menu();
void ensure_file() {
    if (GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES) write_config_file(g_ini, g.cfg);
}

void saved(const std::wstring& what) {
    g.ini_time = file_time(g_ini);
    note(sub(tr(S_note_saved), {{L"what", what}}));
}

void set_deadzone(int hand, float v) {
    v = std::round(std::fmax(0.0f, std::fmin(0.95f, v)) * 100) / 100;
    g.cfg.hand[hand].deadzone = v;
}

void set_average(int hand, float ms) {
    int v = (int)std::lround(std::fmax(0.0f, std::fmin((float)kMaxAverageMs, ms)) / 5.0f) * 5;
    g.cfg.hand[hand].average_ms = v;
}

void commit_average(int hand) {
    ensure_file();
    write_average(g_ini, hand, g.cfg.hand[hand].average_ms);
    saved(tr(hand ? S_what_right_smoothing : S_what_left_smoothing));
}

// slider position (0..1) to the dragged setting
void set_dragged(float fraction) {
    if (g.drag_average) set_average(g.drag_hand, fraction * kMaxAverageMs);
    else set_deadzone(g.drag_hand, fraction * 0.95f);
}

void commit_deadzone(int hand) {
    ensure_file();
    write_deadzone(g_ini, hand, g.cfg.hand[hand].deadzone);
    saved(tr(hand ? S_what_right_deadzone : S_what_left_deadzone));
}

void copy_diagnostics() {
    std::wstring d = std::wstring(L"ancora ") + ANCORA_VERSION + L" diagnostics\r\nconfig: " + g_ini + L"\r\nlog: " + g.log_path + L"\r\n";
    d += g.steamvr ? L"SteamVR running\r\n" : L"SteamVR off\r\n";
    d += g.registered ? L"driver registered\r\n" : L"driver not registered\r\n";
    d += L"language " + g.lang.info.code + L" (setting " + g.cfg.language + L"), " +
         std::to_wstring(g.lang.missing()) + L" strings missing" + (g.lang.error.empty() ? std::wstring() : L", " + g.lang.error) + L"\r\n";
    for (int i = 0; i < 2; ++i) {
        const AncoraHandState& h = g_live.snap.hand[i];
        d += std::wstring(kHandKey[i]) + L": " + widen(h.model) +
             fmt2(L" tracked=%.0f touch=%.0f", h.tracked, h.touch) + fmt1(L" sensor_ignored=%.0f", h.gate_suspect) +
             L"\r\n";
    }
    for (auto& l : g.log.lines) d += l + L"\r\n";
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (d.size() + 1) * sizeof(wchar_t));
        std::memcpy(GlobalLock(mem), d.c_str(), (d.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
        note(tr(S_note_copied));
    }
}

void activate(Id id, int hand) {
    switch (id) {
    case ID_NAV_STATUS: g.page = PAGE_STATUS; break;
    case ID_NAV_TUNE:   g.page = PAGE_TUNE; break;
    case ID_NAV_DIAG:   g.page = PAGE_DIAG; break;
    case ID_MASTER:
        ensure_file();
        g.cfg.enable = !g.cfg.enable;
        write_bool(g_ini, L"enable", g.cfg.enable);
        saved(tr(S_what_anchoring));
        break;
    case ID_CORRECT:
        ensure_file();
        g.cfg.hand[hand].enable = !g.cfg.hand[hand].enable;
        write_bool(g_ini, kHandKey[hand], g.cfg.hand[hand].enable);
        saved(tr(hand ? S_hand_right : S_hand_left));
        break;
    case ID_GATE:
        ensure_file();
        g.cfg.hand[hand].touch_gate = !g.cfg.hand[hand].touch_gate;
        write_bool(g_ini, hand_key(hand, L"_touch_gate"), g.cfg.hand[hand].touch_gate);
        saved(tr(hand ? S_what_right_gate : S_what_left_gate));
        break;
    case ID_TUNE_HAND:  if (g_tune.hand != hand) tune_reset(hand); break;
    case ID_TUNE_START: tune_start(); break;
    case ID_TUNE_RESET: tune_reset(g_tune.hand); break;
    case ID_TUNE_APPLY:
        set_deadzone(g_tune.hand, g_tune.result.suggested);
        commit_deadzone(g_tune.hand);
        break;
    case ID_TUNE_EDGE: tune_start_edge(); break;
    case ID_TUNE_EDGE_FINISH: tune_finish_edge_early(); break;
    case ID_EDGE_ON:
        if (!g.cfg.hand[hand].edge.on) {   // nothing measured yet: open the Tune page on this hand
            g.page = PAGE_TUNE;
            if (g_tune.hand != hand || !tune_measuring()) tune_reset(hand);
            break;
        }
        ensure_file();
        g.cfg.hand[hand].edge_on = !g.cfg.hand[hand].edge_on;
        write_bool(g_ini, hand_key(hand, L"_edge_on"), g.cfg.hand[hand].edge_on);
        saved(tr(hand ? S_what_right_edge : S_what_left_edge));
        break;
    case ID_TUNE_EDGE_APPLY:
    case ID_TUNE_EDGE_CLEAR: {
        int i = g_tune.hand;
        g.cfg.hand[i].edge = id == ID_TUNE_EDGE_APPLY ? g_tune.edge_result : EdgeReach{};
        ensure_file();
        write_edge(g_ini, i, g.cfg.hand[i].edge);
        if (id == ID_TUNE_EDGE_APPLY && !g.cfg.hand[i].edge_on) {   // a fresh measurement is meant to be used
            g.cfg.hand[i].edge_on = true;
            write_bool(g_ini, hand_key(i, L"_edge_on"), true);
        }
        saved(tr(i ? S_what_right_edge : S_what_left_edge));
        if (id == ID_TUNE_EDGE_APPLY) tune_reset(i);
        break;
    }
    case ID_LANG:     language_menu(); break;
    case ID_COPY:     copy_diagnostics(); break;
    case ID_OPEN_CFG: ensure_file(); ShellExecuteW(g.hwnd, L"open", L"notepad.exe", g_ini.c_str(), nullptr, SW_SHOW); break;
    case ID_OPEN_LOG:
        if (g.log_path.empty()) note(tr(S_note_no_log));
        else ShellExecuteW(g.hwnd, L"open", L"notepad.exe", g.log_path.c_str(), nullptr, SW_SHOW);
        break;
    default: break;
    }
}

const Region* hit_at(float x, float y) {
    for (auto it = g_regions.rbegin(); it != g_regions.rend(); ++it)
        if (inside(it->r, x, y)) return &*it;
    return nullptr;
}

void move_focus(int step) {
    if (g_regions.empty()) return;
    int n = (int)g_regions.size(), cur = -1;
    for (int i = 0; i < n; ++i)
        if (g_regions[i].id == g_focus && g_regions[i].hand == g_focus_hand) cur = i;
    int next = cur < 0 ? 0 : (cur + step + n) % n;
    g_focus = g_regions[next].id;
    g_focus_hand = g_regions[next].hand;
}

// ---- window --------------------------------------------------------------------------------------
// pixels per layout unit, from the real client size, so drawing and hit testing always agree
float ui_scale() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    return rc.right > 0 ? rc.right / kW : g.dpi / 96.0f;
}

void create_target() {
    if (g.rt) return;
    RECT rc; GetClientRect(g.hwnd, &rc);
    // software target: no GPU driver stack in the process, far less memory for a small 2D panel
    g.d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                      D2D1::PixelFormat(), 96 * ui_scale(), 96 * ui_scale()),
                                  D2D1::HwndRenderTargetProperties(g.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g.rt);
    g.rt->CreateSolidColorBrush(rgb(kText), &g.br);
}

void discard_target() {
    if (g.br) { g.br->Release(); g.br = nullptr; }
    if (g.rt) { g.rt->Release(); g.rt = nullptr; }
}

void paint() {
    create_target();
    if (!g.rt) { trace("paint: no render target"); return; }
    g.rt->BeginDraw();
    draw(g.rt);
    if (g.rt->EndDraw() == D2DERR_RECREATE_TARGET) discard_target();
}

void tray_add() {
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_TRAY;
    g.nid.hIcon = (HICON)GetClassLongPtrW(g.hwnd, GCLP_HICONSM);
    wcscpy_s(g.nid.szTip, L"ancora");
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

void show_panel() {
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
    SetTimer(g.hwnd, kTimerFrame, kFrameMs, nullptr);
}

void hide_panel() {
    ShowWindow(g.hwnd, SW_HIDE);
    KillTimer(g.hwnd, kTimerFrame);   // no drawing while hidden
    discard_target();                 // drop the back buffer and trim while in the tray
    if (tune_measuring()) tune_reset(g_tune.hand);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

void redraw() { InvalidateRect(g.hwnd, nullptr, FALSE); }

// language menu under the rail button: Automatic, then every language by its own name
void language_menu() {
    g.langs = lang_list();
    HMENU menu = CreatePopupMenu();
    UINT mf = MF_STRING | (g.rtl ? MFT_RIGHTORDER | MFT_RIGHTJUSTIFY : 0);
    std::wstring auto_code = lang_resolve(g.langs, L"auto"), auto_name = auto_code;
    for (auto& li : g.langs) if (li.code == auto_code) auto_name = li.name;
    bool is_auto = _wcsicmp(g.cfg.language.c_str(), L"auto") == 0;
    AppendMenuW(menu, mf | (is_auto ? MF_CHECKED : 0), 1000, sub(tr(S_lang_auto), {{L"name", auto_name}}).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g.langs.size(); ++i) {
        bool cur = !is_auto && _wcsicmp(g.langs[i].code.c_str(), g.lang.info.code.c_str()) == 0;
        AppendMenuW(menu, mf | (cur ? MF_CHECKED : 0), 1001 + (UINT)i, g.langs[i].name.c_str());
    }
    // open above the button, which sits low in the rail
    float sc = ui_scale();
    // the button's outer edge: left in a left to right page, right when mirrored
    float edge = g.rtl ? kW - g.lang_button.left : g.lang_button.left;
    POINT pt = {(LONG)(edge * sc), (LONG)(g.lang_button.top * sc)};
    ClientToScreen(g.hwnd, &pt);
    UINT flags = TPM_RETURNCMD | TPM_BOTTOMALIGN | (g.rtl ? TPM_RIGHTALIGN | TPM_LAYOUTRTL : TPM_LEFTALIGN);
    int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd < 1000) return;
    std::wstring pick = cmd == 1000 ? L"auto" : g.langs[cmd - 1001].code;
    ensure_file();
    write_key(g_ini, L"language", pick);
    g.cfg.language = pick;
    apply_language(pick);
    saved(tr(S_what_language));
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER:
        if (wp == kTimerFrame && refresh_fast()) redraw();
        if (wp == kTimerSlow) { refresh_slow(); if (IsWindowVisible(h)) redraw(); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        discard_target();   // recreated at the new scale on the next paint
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        discard_target();
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lp) / ui_scale(), y = GET_Y_LPARAM(lp) / ui_scale();
        if (g.rtl) x = kW - x;
        if (g.drag_hand >= 0) {
            set_dragged((x - g.drag_track.left) / (g.drag_track.right - g.drag_track.left));
            redraw();
            return 0;
        }
        SetCursor(LoadCursor(nullptr, hit_at(x, y) ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / ui_scale(), y = GET_Y_LPARAM(lp) / ui_scale();
        if (g.rtl) x = kW - x;
        const Region* r = hit_at(x, y);
        trace("down %.0f,%.0f id=%d hand=%d", x, y, r ? (int)r->id : 0, r ? r->hand : -1);
        g_focus_visible = false;
        if (r) {
            g_focus = r->id;
            g_focus_hand = r->hand;
            if (r->id == ID_SLIDER || r->id == ID_AVERAGE) {
                g.drag_hand = r->hand;
                g.drag_average = r->id == ID_AVERAGE;
                g.drag_track = {r->r.left + 12, r->r.top, r->r.right - 12, r->r.bottom};
                SetCapture(h);
                set_dragged((x - g.drag_track.left) / (g.drag_track.right - g.drag_track.left));
            } else {
                activate(r->id, r->hand);
            }
        }
        redraw();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g.drag_hand >= 0) {
            int hand = g.drag_hand;
            g.drag_hand = -1;
            ReleaseCapture();
            if (g.drag_average) commit_average(hand);
            else commit_deadzone(hand);
            redraw();
        }
        return 0;
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0;
        trace("key %u focus=%d hand=%d regions=%zu", (unsigned)wp, (int)g_focus, g_focus_hand, g_regions.size());
        g_focus_visible = true;
        if (wp == VK_TAB) move_focus(shift ? -1 : 1);
        else if (wp == VK_DOWN) move_focus(1);
        else if (wp == VK_UP) move_focus(-1);
        else if (wp == VK_LEFT || wp == VK_RIGHT) {
            bool more = (wp == VK_RIGHT) != g.rtl;
            if (g_focus == ID_SLIDER) {
                set_deadzone(g_focus_hand, g.cfg.hand[g_focus_hand].deadzone + (more ? 0.05f : -0.05f));
                commit_deadzone(g_focus_hand);
            } else if (g_focus == ID_AVERAGE) {
                set_average(g_focus_hand, g.cfg.hand[g_focus_hand].average_ms + (more ? 25.0f : -25.0f));
                commit_average(g_focus_hand);
            } else {
                move_focus(more ? 1 : -1);
            }
        } else if (wp == VK_RETURN || wp == VK_SPACE) {
            if (g_focus != ID_SLIDER && g_focus != ID_AVERAGE) activate(g_focus, g_focus_hand);
        } else if (wp == VK_ESCAPE) {
            if (g.page != PAGE_STATUS) { g.page = PAGE_STATUS; g_focus = ID_NAV_STATUS; g_focus_hand = 0; }
            else hide_panel();
        }
        redraw();
        return 0;
    }
    case WM_INPUTLANGCHANGE:   // keyboard layout switched (Win+Space): key hints follow it
        read_key_names();
        redraw();
        break;                 // DefWindowProc passes it on to child windows
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) { IsWindowVisible(h) ? hide_panel() : show_panel(); }
        if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            UINT mf = MF_STRING | (g.rtl ? MFT_RIGHTORDER | MFT_RIGHTJUSTIFY : 0);
            AppendMenuW(menu, mf, 1, tr(S_tray_open).c_str());
            AppendMenuW(menu, mf, 2, tr(S_tray_exit).c_str());
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | (g.rtl ? TPM_LAYOUTRTL : 0), pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) show_panel();
            if (cmd == 2) { g.exiting = true; DestroyWindow(h); }
        }
        return 0;
    case WM_CLOSE:
        // wParam 1: really exit (SteamVR quit, or the installer closing a running panel)
        if (wp == 1 || g.exiting) { DestroyWindow(h); return 0; }
        hide_panel();   // close button keeps ancora in the tray
        return 0;
    // logoff, shutdown, and installers through the Restart Manager (ENDSESSION_CLOSEAPP):
    // agree, then exit, so the files are free and no stale tray icon is left behind
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { g.exiting = true; DestroyWindow(h); }
        return 0;
    case WM_DESTROY:
        edge_sampler_stop();
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---- language -------------------------------------------------------------------------------
Lang load_lang(const std::wstring& code) {
    Lang l;
    if (code == L"qps-ploc" || code == L"qps-plocm") { lang_pseudo(l, g.en, code == L"qps-plocm"); return l; }
    for (auto& li : g.langs)
        if (_wcsicmp(li.code.c_str(), code.c_str()) == 0) {
            l.info = li;
            lang_parse(lang_bytes(li), l.info, &l);
            return l;
        }
    l.info.code = code;
    l.error = L"no language file";
    return l;
}

void make_formats() {
    IDWriteTextFormat** all[] = {&g.f_title, &g.f_logo, &g.f_nav, &g.f_h2, &g.f_big, &g.f_body,
                                 &g.f_small, &g.f_label, &g.f_mono, &g.f_mono_big, &g.f_path};
    for (auto f : all) if (*f) { (*f)->Release(); *f = nullptr; }
    g.f_title    = make_format(28, DWRITE_FONT_WEIGHT_BOLD);
    g.f_logo     = make_format(24, DWRITE_FONT_WEIGHT_BOLD);
    g.f_nav      = make_format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.f_h2       = make_format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.f_big      = make_format(26, DWRITE_FONT_WEIGHT_BOLD);
    g.f_body     = make_format(15, DWRITE_FONT_WEIGHT_NORMAL);
    g.f_small    = make_format(13, DWRITE_FONT_WEIGHT_NORMAL);
    g.f_label    = make_format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.f_mono     = make_format(14, DWRITE_FONT_WEIGHT_NORMAL, true);
    g.f_mono_big = make_format(26, DWRITE_FONT_WEIGHT_NORMAL, true);
    // Japanese fonts (Yu Gothic, Meiryo) draw the backslash as a yen sign and Korean
    // ones (Malgun Gothic) as a won sign, so a path in the language's font shows those
    // between its folders; paths use Segoe UI in every language
    g.f_path     = make_format(13, DWRITE_FONT_WEIGHT_NORMAL, false, L"Segoe UI");
}

// setting: "auto" or a language code, as in the ini; also reloads the files, so an
// edited or new file in the lang folder shows on the next pick from the menu
void apply_language(const std::wstring& setting) {
    if (g.en.info.code.empty()) {
        g.en = load_lang(L"en");
        for (int k = 0; k < S_COUNT; ++k)   // no English file at all: show the keys
            if (g.en.s[k].empty()) g.en.s[k] = widen(kStrKey[k]);
    }
    std::wstring code = lang_resolve(g.langs, setting);
    g.lang = code == L"en" ? g.en : load_lang(code);
    g.lang_setting = setting;
    g.rtl = g.lang.info.rtl;
    make_formats();
    trace("language %ls -> %ls rtl=%d missing=%d", setting.c_str(), code.c_str(), (int)g.rtl, g.lang.missing());
}

void create_resources() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g.dw);
    float dashes[2] = {3, 3};
    g.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                             dashes, 2, &g.dash);
    g.langs = lang_list();
    apply_language(L"auto");
    read_key_names();
}

// anchor mark rendered to an HICON of px by px; used for the window, taskbar and tray
// the anchor mark on its dark disc, px by px, premultiplied BGRA
IWICBitmap* render_mark(IWICImagingFactory* wic, int px) {
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    g.d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                           D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                           96, 96), &rt);
    ID2D1SolidColorBrush* saved_br = g.br;
    rt->CreateSolidColorBrush(rgb(kText), &g.br);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    float c = px / 2.0f;
    circle(rt, c, c, c - 0.5f, 0x0B0F14);
    // small sizes: anchor fills the disc so it stays readable in the tray and title bar
    draw_mark(rt, c, c, c * (px <= 32 ? 0.98f : 0.82f), px <= 16 ? 2.0f : px <= 32 ? 1.5f : 1.2f);
    rt->EndDraw();
    g.br->Release();
    g.br = saved_br;
    rt->Release();
    return bmp;
}

HICON make_icon(int px) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return nullptr;
    IWICBitmap* bmp = render_mark(wic, px);
    std::vector<uint32_t> pixels((size_t)px * px);
    bmp->CopyPixels(nullptr, px * 4, (UINT)(pixels.size() * 4), (BYTE*)pixels.data());
    bmp->Release();
    wic->Release();
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi); bi.bV5Width = px; bi.bV5Height = -px; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    std::memcpy(bits, pixels.data(), pixels.size() * 4);
    HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---- png output (screenshot and icon sheet) --------------------------------------------------
bool save_png(IWICImagingFactory* wic, IWICBitmap* bmp, UINT W, UINT H, const std::wstring& out) {
    IWICStream* st = nullptr;
    wic->CreateStream(&st);
    if (FAILED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE))) return false;
    IWICBitmapEncoder* enc = nullptr;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(st, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* fr = nullptr;
    enc->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(W, H);
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppPBGRA;
    fr->SetPixelFormat(&pf);
    fr->WriteSource(bmp, nullptr);
    fr->Commit();
    enc->Commit();
    fr->Release(); enc->Release(); st->Release();
    return true;
}

// ---- screenshot (self test) --------------------------------------------------------------------
// --kbd: render the key hints for another keyboard layout (a KLID such as 00000407,
// German). Active for this thread only; a layout the user did not have is unloaded again.
struct KbdOverride {
    HKL hkl = nullptr;
    bool loaded = false;
    explicit KbdOverride(const std::wstring& klid) {
        if (klid.empty()) return;
        HKL before[64];
        int n = GetKeyboardLayoutList(64, before);
        hkl = LoadKeyboardLayoutW(klid.c_str(), KLF_NOTELLSHELL);
        if (!hkl) return;
        loaded = std::find(before, before + n, hkl) == before + n;
        ActivateKeyboardLayout(hkl, 0);
    }
    ~KbdOverride() { if (loaded) UnloadKeyboardLayout(hkl); }
};

int screenshot(const std::wstring& out, const std::wstring& page, const std::wstring& lang, const std::wstring& kbd) {
    KbdOverride layout(kbd);
    create_resources();
    g.cfg = read_config(g_ini);
    g.ini_time = file_time(g_ini);   // refresh_slow below must not re-read it and undo --lang
    apply_language(lang.empty() ? g.cfg.language : lang);
    g.page = page == L"tune" || page == L"edge" ? PAGE_TUNE : page == L"diag" ? PAGE_DIAG : PAGE_STATUS;
    if (g_fake) {
        g.cfg.enable = true; g.cfg.hand[0] = {true, true, 0.0f, 0}; g.cfg.hand[1] = {true, false, 0.20f, 100};
        g.steamvr = true; g.registered = true;
        uint64_t base = 50000;
        for (int k = 0; k < 60; ++k) {   // two seconds of history at 30 Hz
            fake_state(base + k * 33);
            for (int i = 0; i < 2; ++i) push_trail(i, g_live.snap.hand[i].raw_x, g_live.snap.hand[i].raw_y, g_live.snap.tick_ms);
        }
        g_live.rate[0] = 498; g_live.rate[1] = 501;
        g.log.tables = 5; g.log.generation = 3; g.log.replayed = 27;
        g.log.gen_time = L"20:34:04"; g.log.config_time = L"20:34:04";
        g.log.lines = {L"20:12:31  found IVRDriverInput_005", L"20:12:31  patched 5 input interface table(s)",
                       L"20:34:04  config enable=1 left=1 gate=1 dz=0.00 right=1 gate=0 dz=0.20",
                       L"20:34:04  tracking left /input/joystick/x of Oculus Quest3 (Left Controller)",
                       L"20:34:04  tracking right /input/joystick/x of Oculus Quest3 (Right Controller)",
                       L"20:34:04  logic loaded, generation 3, replayed 27 component(s)"};
        // a finished tune of the drifting left stick
        g_tune.hand = 0;
        float px[12] = {.11f, .15f, .07f, .17f, .12f, .05f, .2f, .09f, .14f, .03f, .17f, .08f};
        float py[12] = {.31f, .37f, .27f, .33f, .4f, .21f, .38f, .35f, .29f, .16f, .41f, .31f};
        for (int k = 0; k < 12; ++k) g_tune.pts.push_back({px[k], py[k]});
        g_tune.result = tune_compute(g_tune.pts, 0, 300, true);
        g_tune.stage = TUNE_REVIEW;
        g_tune.flicks = kFlicks;
        {   // a finished edge measurement of a right stick short toward forward left, saved for the right hand
            Tune keep = g_tune;
            g_tune = Tune{};
            g_tune.hand = 1;
            for (int b = 0; b < kEdgeBins; ++b) {
                float deg = b * 360.0f / kEdgeBins, d = std::fabs(deg - 150.0f);
                if (d > 180) d = 360 - d;
                g_tune.edge.bin[b] = d >= 80 ? 0.999f : 0.65f + 0.349f * d / 80.0f;
            }
            for (int b = 0; b < kEdgeBins; ++b) { g_tune.edge.passes[b] = kEdgePasses; g_tune.edge.start[b] = g_tune.edge.bin[b]; }
            g_tune.edge_result = g_tune.edge.result();
            g_tune.stage = TUNE_EDGE_REVIEW;
            g.cfg.hand[1].edge = g_tune.edge_result;
            if (page != L"edge") g_tune = keep;
        }
        saved(tr(S_what_right_deadzone));
        g.note_until = ~0ull;
    } else {
        refresh_slow();
        live_poll();
    }
    UINT W = (UINT)(kW * 1.5f), H = (UINT)(kH * 1.5f);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    g.d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                           D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                           144, 144), &rt);
    rt->CreateSolidColorBrush(rgb(kText), &g.br);
    rt->BeginDraw();
    draw(rt);
    HRESULT hr = rt->EndDraw();
    bool ok = save_png(wic, bmp, W, H, out);
    std::wprintf(L"screenshot %ls (%ux%u) language %ls draw hr=0x%08lx\n", out.c_str(), W, H,
                 g.lang.info.code.c_str(), (unsigned long)hr);
    if (!g.lang.error.empty()) std::wprintf(L"language error: %ls\n", g.lang.error.c_str());
    {
        std::wstring k = L"keys: " + g.key_tab + L" / " + g.key_enter + L" / " + g.key_esc;
        std::string u(WideCharToMultiByte(CP_UTF8, 0, k.c_str(), (int)k.size(), nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, k.c_str(), (int)k.size(), u.data(), (int)u.size(), nullptr, nullptr);
        std::printf("%s\n", u.c_str());
    }
    for (auto& o : g.overflow) {   // console output is UTF-8, so any script prints
        std::string u(WideCharToMultiByte(CP_UTF8, 0, o.c_str(), (int)o.size(), nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, o.c_str(), (int)o.size(), u.data(), (int)u.size(), nullptr, nullptr);
        std::printf("overflow: %s\n", u.c_str());
    }
    return SUCCEEDED(hr) && ok ? 0 : 1;
}

// .ico with PNG images at the sizes Windows asks for (Explorer, Settings > Apps,
// taskbar); build_driver.bat embeds it into ancora.exe as resource 1
int write_ico(const std::wstring& out) {
    create_resources();
    const int sizes[6] = {16, 24, 32, 48, 64, 256};
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 1;
    std::vector<std::vector<BYTE>> pngs;
    for (int px : sizes) {
        IWICBitmap* bmp = render_mark(wic, px);
        IStream* mem = nullptr;
        CreateStreamOnHGlobal(nullptr, TRUE, &mem);
        IWICStream* st = nullptr;
        wic->CreateStream(&st);
        st->InitializeFromIStream(mem);
        IWICBitmapEncoder* enc = nullptr;
        wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        enc->Initialize(st, WICBitmapEncoderNoCache);
        IWICBitmapFrameEncode* fr = nullptr;
        enc->CreateNewFrame(&fr, nullptr);
        fr->Initialize(nullptr);
        fr->SetSize(px, px);
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;   // PNG stores straight alpha
        fr->SetPixelFormat(&pf);
        IWICFormatConverter* conv = nullptr;
        wic->CreateFormatConverter(&conv);
        conv->Initialize(bmp, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        fr->WriteSource(conv, nullptr);
        fr->Commit();
        enc->Commit();
        STATSTG stat{};
        mem->Stat(&stat, STATFLAG_NONAME);
        std::vector<BYTE> data((size_t)stat.cbSize.QuadPart);
        LARGE_INTEGER zero{};
        mem->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG rd = 0;
        mem->Read(data.data(), (ULONG)data.size(), &rd);
        pngs.push_back(std::move(data));
        conv->Release(); fr->Release(); enc->Release(); st->Release(); mem->Release(); bmp->Release();
    }
    wic->Release();

    // ICONDIR, one ICONDIRENTRY per image, then the PNG data
    std::vector<BYTE> ico;
    auto put16 = [&](uint16_t v) { ico.push_back(v & 255); ico.push_back(v >> 8); };
    auto put32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) ico.push_back((v >> (8 * k)) & 255); };
    put16(0); put16(1); put16((uint16_t)pngs.size());
    uint32_t offset = 6 + 16 * (uint32_t)pngs.size();
    for (size_t i = 0; i < pngs.size(); ++i) {
        ico.push_back(sizes[i] >= 256 ? 0 : (BYTE)sizes[i]);   // 0 means 256
        ico.push_back(sizes[i] >= 256 ? 0 : (BYTE)sizes[i]);
        ico.push_back(0); ico.push_back(0);                     // no palette, reserved
        put16(1); put16(32);                                    // planes, bits per pixel
        put32((uint32_t)pngs[i].size());
        put32(offset);
        offset += (uint32_t)pngs[i].size();
    }
    for (auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
    HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    DWORD wr = 0;
    WriteFile(h, ico.data(), (DWORD)ico.size(), &wr, nullptr);
    CloseHandle(h);
    return wr == ico.size() ? 0 : 1;
}

int icon_sheet(const std::wstring& out) {
    create_resources();
    const int sizes[4] = {16, 32, 48, 256};
    UINT W = 16 + 32 + 48 + 256 + 50, H = 256;
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    IWICBitmap* sheet = nullptr;
    wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &sheet);
    WICRect all{0, 0, (INT)W, (INT)H};
    IWICBitmapLock* lock = nullptr;
    sheet->Lock(&all, WICBitmapLockWrite, &lock);
    UINT stride = 0, size = 0; BYTE* dst = nullptr;
    lock->GetStride(&stride); lock->GetDataPointer(&size, &dst);
    for (UINT y = 0; y < H; ++y)                    // dark grey sheet, like a taskbar
        for (UINT x = 0; x < W; ++x) ((uint32_t*)(dst + y * stride))[x] = 0xFF2B2B2B;
    int ox = 10;
    for (int px : sizes) {
        HICON ic = make_icon(px);
        ICONINFO ii{}; GetIconInfo(ic, &ii);
        std::vector<uint32_t> buf((size_t)px * px);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = px;
        bi.bmiHeader.biHeight = -px; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, ii.hbmColor, 0, px, buf.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        for (int y = 0; y < px; ++y)
            for (int x = 0; x < px; ++x) {
                uint32_t sp = buf[(size_t)y * px + x], a = sp >> 24;
                uint32_t& d = ((uint32_t*)(dst + (H - px + y) * stride))[ox + x];
                uint32_t r = 0;
                for (int ch = 0; ch < 24; ch += 8)        // premultiplied over the sheet
                    r |= ((((sp >> ch) & 255) + ((d >> ch) & 255) * (255 - a) / 255) & 255) << ch;
                d = 0xFF000000 | r;
            }
        DeleteObject(ii.hbmColor); DeleteObject(ii.hbmMask); DestroyIcon(ic);
        ox += px + 10;
    }
    lock->Release();
    return save_png(wic, sheet, W, H, out) ? 0 : 1;
}

// ---- self test: ini round trip and tune math --------------------------------------------------
int selftest() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring p = std::wstring(tmp) + L"ancora_selftest.ini";
    DeleteFileW(p.c_str());
    int fails = 0, count = 0;
    auto check = [&](bool ok, const char* what) {
        ++count;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++fails;
    };

    Config c = read_config(p);
    check(!c.exists && !c.enable && c.hand[0].enable && c.hand[1].enable && c.hand[0].touch_gate &&
          c.hand[1].touch_gate && c.hand[0].deadzone == 0 && c.hand[1].deadzone == 0,
          "missing file reads safe defaults (disabled, both sticks set to be corrected)");
    Config d; d.enable = true; d.hand[1] = {true, false, 0.2f, 120};
    write_config_file(p, d);
    c = read_config(p);
    check(c.exists && c.enable && c.hand[1].enable && !c.hand[1].touch_gate && std::fabs(c.hand[1].deadzone - 0.2f) < 1e-4 &&
          c.hand[1].average_ms == 120 && c.hand[0].average_ms == 0 &&
          c.hand[0].touch_gate && c.hand[0].deadzone == 0, "full file write and read back, hands independent");
    write_key(p, L"right_average_ms", L"5000");
    check(read_config(p).hand[1].average_ms == kMaxAverageMs, "out of range average clamps to 300 ms");
    write_key(p, L"right_average_ms", L"-5");
    check(read_config(p).hand[1].average_ms == 0, "negative average reads as off");
    write_bool(p, L"left_touch_gate", false);
    write_deadzone(p, 0, 0.35f);
    c = read_config(p);
    check(!c.hand[0].touch_gate && std::fabs(c.hand[0].deadzone - 0.35f) < 1e-4 &&
          std::fabs(c.hand[1].deadzone - 0.2f) < 1e-4 && c.enable, "left key writes leave right and other keys alone");
    std::string body = read_file(p);
    check(body.find("; master switch") != std::string::npos, "comments survive key writes");
    write_key(p, L"right_deadzone", L"7");
    check(std::fabs(read_config(p).hand[1].deadzone - 0.95f) < 1e-4, "out of range deadzone clamps to 0.95");
    write_key(p, L"right_deadzone", L"abc");
    check(read_config(p).hand[1].deadzone == 0.0f, "garbage deadzone reads as 0");
    write_bool(p, L"right_touch_gate", true);
    write_key(p, L"touch_gate", L"0");
    write_key(p, L"deadzone", L"0.5");
    c = read_config(p);
    check(c.hand[1].touch_gate && c.hand[1].deadzone == 0.0f, "old shared keys touch_gate and deadzone are ignored");
    DeleteFileW(p.c_str());

    // tune math: forward drift peaking at 0.41 -> suggestion 0.45, direction forward
    std::vector<Point> pts = {{0.0f, 0.2f}, {0.05f, 0.3f}, {0.0f, 0.41f}, {-0.03f, 0.25f}, {0.02f, 0.22f}};
    TuneResult r = tune_compute(pts, 0, 100, true);
    check(std::fabs(r.peak - 0.41f) < 1e-3, "tune peak is the largest resting magnitude");
    check(std::fabs(r.suggested - 0.45f) < 1e-4, "tune suggests peak plus margin rounded up to 5 %");
    check(r.direction == S_dir_forward, "tune direction of forward drift is forward");
    check(r.gate_ok, "tune: thumb never on while let go -> gate alone stops it");
    check(!tune_compute(pts, 30, 100, true).gate_ok, "tune: thumb on 30 % of the time -> gate not enough");
    check(!tune_compute(pts, 0, 100, false).gate_ok, "tune: no touch sensor -> gate not enough");
    check(tune_compute({{0.0f, 0.95f}}, 0, 10, true).suggested <= 0.95f, "tune suggestion never above 0.95");

    // the edge stretch math, measuring, learning and smoothing are tested in
    // test\ancora_test.cpp; here: the Tune page flow and the ini keys
    {
        const float pi = 3.14159265f;
        auto reach_at = [&](float deg) {   // simulated worn stick: 0.999, dipping to 0.65 at 135 deg
            float d = std::fabs(deg - 135.0f);
            if (d > 180) d = 360 - d;
            return d >= 90 ? 0.999f : 0.65f + (0.999f - 0.65f) * d / 90.0f;
        };
        tune_reset(1);
        tune_start_edge(false);
        for (int k = 0; k < 1200 && g_tune.stage == TUNE_EDGE; ++k) {   // up to 13 turns, 4 deg a frame
            float deg = std::fmod(k * 4.0f, 360.0f), m = reach_at(deg);
            tune_feed_edge(m * std::cos(deg * pi / 180), m * std::sin(deg * pi / 180));
        }
        check(g_tune.stage == TUNE_EDGE_REVIEW && g_tune.edge_result.on, "tune edge: finishes once every direction has settled");
        const EdgeReach e = g_tune.edge_result;
        int sk = edge_shortest(e);
        check(sk == kEdgeSectors * 3 / 8 && std::fabs(e.r[sk] - 0.65f) < 0.02f, "tune edge: review shows the shortest reach, 0.65 at 135 deg");
        tune_reset(1);
        tune_start_edge(false);
        for (int k = 0; k < 300; ++k) tune_feed_edge(0.9f * std::cos(k * 0.05f), 0.0f);   // left and right only
        check(g_tune.stage == TUNE_EDGE, "tune edge: never finishes without reaching every direction");
        tune_reset(0);
        write_edge(p, 1, e);
        check(read_config(p).hand[1].edge.on && !read_config(p).hand[0].edge.on, "edge: ini write and read back per hand");
        write_edge(p, 1, EdgeReach{});
        check(!read_config(p).hand[1].edge.on, "edge: cleared edge reads as off");
        check(read_config(p).hand[1].edge_on, "edge: switch reads on when the key is missing");
        write_edge(p, 1, e);
        write_bool(p, L"right_edge_on", false);
        c = read_config(p);
        check(c.hand[1].edge.on && !c.hand[1].edge_on, "edge: switched off keeps the measured values");
        DeleteFileW(p.c_str());
    }

    // tune run on a simulated stick that never centers: it creeps forward and jitters
    // every frame, and after each flick it settles off center, never still
    uint32_t seed = 12345;
    auto noise = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xFFFF) / 65535.0f * 0.02f - 0.01f; };
    tune_reset(0);
    g_tune.stage = TUNE_REST;
    g_tune.t0 = 0;
    uint64_t now = 0;
    for (; now <= kRestMs + 33; now += 33) tune_feed(0.10f + noise(), 0.30f + now * 0.000004f + noise(), false, true, now);
    check(g_tune.stage == TUNE_FLICK && g_tune.pts.size() >= 40, "tune rest stage records about 10 points a second");
    for (int f = 0; f < kFlicks; ++f) {
        for (int k = 0; k < 6; ++k, now += 33) tune_feed(0.0f, 0.97f, true, true, now);          // full push
        for (int k = 0; k < 45; ++k, now += 33)                                                   // let go, creeping
            tune_feed(0.12f + noise(), 0.36f + k * 0.0015f + noise(), false, true, now);
    }
    check(g_tune.stage == TUNE_REVIEW && g_tune.flicks == kFlicks, "tune finishes all flicks on a stick that never centers");
    check(g_tune.result.peak > 0.3f && g_tune.result.peak < 0.5f && g_tune.result.direction == S_dir_forward,
          "tune result on the drifting stick: peak about 0.4, forward");
    check(g_tune.result.gate_ok, "tune: thumb was off after every release -> gate alone stops it");
    tune_reset(0);

    // languages: every shipped file parses, has every key and no others, and keeps
    // the English placeholders; English itself is complete
    g.langs = lang_list();
    check(g.langs.size() >= 2, "language files are embedded");
    g.en = load_lang(L"en");
    check(g.en.error.empty() && g.en.missing() == 0 && g.en.unknown.empty(), "English has every string");
    auto u8 = [](const std::wstring& w) {
        std::string u(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), u.data(), (int)u.size(), nullptr, nullptr);
        return u;
    };
    for (auto& li : g.langs) {
        Lang l = load_lang(li.code);
        std::string code = u8(li.code), why;
        if (!l.error.empty()) why += " error: " + u8(l.error);
        for (int k = 0; k < S_COUNT; ++k) {
            if (l.s[k].empty()) why += std::string(" missing ") + kStrKey[k];
            else if (lang_placeholders(l.s[k]) != lang_placeholders(g.en.s[k]))
                why += std::string(" placeholders ") + kStrKey[k];
        }
        for (auto& x : l.unknown) why += " unknown " + u8(x);
        std::string what = "language " + code + " (" + u8(li.name) + (li.rtl ? ", rtl" : "") + ") complete" + why;
        check(why.empty(), what.c_str());
    }
    std::vector<LangInfo> fake(5);
    const wchar_t* codes[5] = {L"en", L"pt-BR", L"zh-Hans", L"zh-Hant", L"he"};
    for (int k = 0; k < 5; ++k) fake[k].code = codes[k];
    check(lang_match(fake, L"pt-PT") == L"pt-BR", "Windows pt-PT picks pt-BR when there is no pt-PT");
    {
        std::vector<LangInfo> pt(2);
        pt[0].code = L"pt-BR"; pt[1].code = L"pt-PT";
        check(lang_match(pt, L"pt-PT") == L"pt-PT" && lang_match(pt, L"pt-BR") == L"pt-BR" && lang_match(pt, L"pt-AO") != L"",
              "Windows pt-PT picks pt-PT, pt-BR picks pt-BR, other Portuguese picks one of them");
    }
    check(lang_match(fake, L"zh-CN") == L"zh-Hans" && lang_match(fake, L"zh-TW") == L"zh-Hant" &&
          lang_match(fake, L"zh-HK") == L"zh-Hant" && lang_match(fake, L"zh-Hans-SG") == L"zh-Hans",
          "Windows Chinese variants pick simplified or traditional");
    check(lang_match(fake, L"he-IL") == L"he" && lang_match(fake, L"xx-YY").empty(), "region tags fall back to the language");
    check(lang_resolve(fake, L"he") == L"he" && lang_resolve(fake, L"qps-plocm") == L"qps-plocm", "explicit setting wins");
    g.rtl = false;
    check(sub(L"{n} of {total}, {n}", {{L"n", L"2"}, {L"total", L"6"}}) == L"2 of 6, 2", "placeholders fill every use");
    g.rtl = true;
    std::wstring isod = sub(L"x {v}", {{L"v", L"0.45"}});
    check(isod == std::wstring(L"x \x200f\x202a") + L"0.45" + L"\x202c", "rtl embeds filled values");
    g.rtl = false;
    Lang ps;
    lang_pseudo(ps, g.en, true);
    check(ps.info.rtl && ps.missing() == 0 && lang_placeholders(ps.s[S_step_flick_push]) ==
          lang_placeholders(g.en.s[S_step_flick_push]), "pseudo mirrored language keeps placeholders");
    std::string bad = "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY a \"aaaa\">]><language code=\"x\"><nav_tune>&a;</nav_tune></language>";
    LangInfo bi;
    Lang bl;
    check(!lang_parse(bad, bi, &bl), "a language file with a DTD is refused");

    // SteamVR registration edits (ancora_steamvr.h), on temp copies of Steam files;
    // their progress messages (including the expected "cannot read" of the broken
    // file case) are kept out of the test log
    {
        svr::quiet() = true;
        svr::Json j;
        std::string src = "\xEF\xBB\xBF{\"a\":[1,-2.5e3,true,null,\"x\\\"\\\\\\u00e9\\ud83d\\ude00\\n\"],\"b\":{},\"c\":[]}";
        check(svr::json_parse(src, j) && j.o.size() == 3 && j.o[0].second.a[1].s == "-2.5e3" &&
              j.o[0].second.a[4].s == "x\"\\\xC3\xA9\xF0\x9F\x98\x80\n", "json reads BOM, numbers as text, escapes and surrogates");
        std::string w;
        svr::json_write(w, j);
        svr::Json back;
        std::string w2;
        check(svr::json_parse(w, back) && (svr::json_write(w2, back), w2 == w) && back.o[0].first == "a" &&
              back.o[2].first == "c", "json write round trips and keeps key order");
        check(!svr::json_parse("{\"a\":1,}", j) && !svr::json_parse("{\"a\":1} x", j) && !svr::json_parse("", j) &&
              !svr::json_parse(std::string(200, '['), j), "broken or too deep json is refused");

        std::wstring dir = std::wstring(tmp) + L"ancora_selftest_steam";
        svr::remove_tree(dir);
        svr::make_dirs(dir);
        std::wstring bak = dir + L"\\backup", app = dir + L"\\appconfig.json", set = dir + L"\\steamvr.vrsettings";
        std::wstring man = L"C:\\Users\\T\\AppData\\Local\\ancora\\ancora.vrmanifest";
        std::string mu = svr::utf8(man);
        auto load = [](const std::wstring& p) { std::string s; svr::Json r; svr::read_all(p, s); svr::json_parse(s, r); return r; };
        auto backups = [&]() { int n = 0; WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((bak + L"\\*.bak").c_str(), &fd);
                               if (f != INVALID_HANDLE_VALUE) { do ++n; while (FindNextFileW(f, &fd)); FindClose(f); } return n; };

        check(svr::appconfig_register(app, man, bak) && load(app).find("manifest_paths")->a.size() == 1 &&
              load(app).find("manifest_paths")->a[0].s == mu && backups() == 0, "register creates a missing appconfig.json");
        svr::write_all(app, "{\"manifest_paths\":[\"D:\\\\other\\\\x.vrmanifest\",\"E:\\\\old\\\\ANCORA.vrmanifest\"],\"keep\":7}");
        check(svr::appconfig_register(app, man, bak), "register edits an existing appconfig.json");
        svr::Json a = load(app);
        check(a.find("manifest_paths")->a.size() == 2 && a.find("manifest_paths")->a[0].s == "D:\\other\\x.vrmanifest" &&
              a.find("manifest_paths")->a[1].s == mu && a.find("keep")->s == "7" && backups() == 1,
              "old ancora manifest replaced, others kept, backup made");
        check(svr::appconfig_register(app, man, bak) && backups() == 1, "repeated register writes nothing");
        svr::write_all(app, "{ broken");
        check(!svr::appconfig_register(app, man, bak) && load(app).type == svr::Json::Null, "unreadable appconfig.json left alone");

        svr::write_all(app, "{\"manifest_paths\":[\"D:\\\\other\\\\x.vrmanifest\",\"" + std::string("C:\\\\a\\\\ancora.vrmanifest") + "\"]}");
        svr::write_all(set, "{\"driver_ancora\":{\"x\":1},\"ancora.panel\":{},\"steamvr\":{\"y\":2},"
                            "\"system.generated.ancora_probe.exe\":{},\"system.generated.probe.exe\":{}}");
        int before = backups();
        auto unapp = [](svr::Json& x) { return svr::appconfig_set(x, std::string()); };
        check(svr::edit_json(set, bak, svr::vrsettings_strip) == svr::Edit::Written &&
              svr::edit_json(app, bak, unapp) == svr::Edit::Written && backups() == before + 2,
              "unregister strips both files with backups");
        svr::Json s = load(set);
        check(s.o.size() == 2 && s.o[0].first == "steamvr" && s.find("steamvr")->find("y")->s == "2" &&
              s.find("system.generated.probe.exe") && load(app).find("manifest_paths")->a.size() == 1,
              "only ancora's entries removed, a plain probe.exe section of another program stays");
        check(svr::edit_json(set, bak, svr::vrsettings_strip) == svr::Edit::Unchanged &&
              svr::edit_json(app, bak, unapp) == svr::Edit::Unchanged && backups() == before + 2,
              "second unregister changes nothing and makes no backup");
        check(svr::edit_json(dir + L"\\missing.json", bak, svr::vrsettings_strip) == svr::Edit::Unchanged,
              "missing Steam file is skipped");
        svr::remove_tree(dir);
        svr::quiet() = false;
    }
    std::printf("%d test(s), %d failure(s)\n", count, fails);
    return fails ? 1 : 0;
}

void attach_console() {
    // stdout already redirected to a file or pipe: keep it
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto has = [&](const wchar_t* a) { for (int i = 1; i < argc; ++i) if (_wcsicmp(argv[i], a) == 0) return i; return 0; };
    auto arg_after = [&](const wchar_t* a, const wchar_t* def) {
        int k = has(a);
        return std::wstring(k && k + 1 < argc ? argv[k + 1] : def);
    };

    g_ini = local_appdata() + L"\\ancora\\ancora.ini";
    g.log_path = steamvr_log_path();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (has(L"--selftest")) { attach_console(); return selftest(); }
    for (const wchar_t* action : {L"check", L"register", L"unregister"}) {
        if (!has((std::wstring(L"--") + action).c_str())) continue;
        attach_console();
        // backups outside the install folder: the uninstaller deletes that folder
        // right after --unregister, and the backups must outlive it
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = arg_after(L"--root", (local_appdata() + L"\\ancora").c_str());
        std::wstring backups = arg_after(L"--backup-dir", (std::wstring(tmp) + L"ancora-backup").c_str());
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        return svr::run(action, root, backups);
    }
    if (has(L"--icon-png")) return icon_sheet(arg_after(L"--icon-png", L"ancora_icon.png"));
    if (has(L"--icon-ico")) return write_ico(arg_after(L"--icon-ico", L"ancora.ico"));
    if (has(L"--screenshot")) {
        attach_console();
        g_fake = has(L"--fake") != 0;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return screenshot(arg_after(L"--screenshot", L"ancora.png"), arg_after(L"--page", L"status"),
                          arg_after(L"--lang", L""), arg_after(L"--kbd", L""));
    }

    // one panel at a time: a second launch just shows the first
    HANDLE single = CreateMutexW(nullptr, TRUE, L"ancora_panel_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"ancora_panel", nullptr)) {
            ShowWindow(other, SW_SHOW);
            SetForegroundWindow(other);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    create_resources();
    g.cfg = read_config(g_ini);
    g.ini_time = file_time(g_ini);
    apply_language(g.cfg.language);
    g.from_steamvr = has(L"--steamvr") != 0;
    bool start_hidden = g.from_steamvr || has(L"--tray");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    HICON icon_big = make_icon(GetSystemMetrics(SM_CXICON));
    HICON icon_small = make_icon(GetSystemMetrics(SM_CXSMICON));
    wc.hIcon = icon_big ? icon_big : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = icon_small ? icon_small : wc.hIcon;
    wc.lpszClassName = L"ancora_panel";
    wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x14, 0x1B));
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"ancora", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                             nullptr, nullptr, inst, nullptr);
    // size for the monitor the window lands on; repeat once if placing it moved it to another dpi
    for (int pass = 0; pass < 2; ++pass) {
        g.dpi = (float)GetDpiForWindow(g.hwnd);
        RECT r = {0, 0, (LONG)(kW * g.dpi / 96), (LONG)(kH * g.dpi / 96)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, (UINT)g.dpi);
        SetWindowPos(g.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        if ((float)GetDpiForWindow(g.hwnd) == g.dpi) break;
    }
    g.dpi = (float)GetDpiForWindow(g.hwnd);
    BOOL dark = TRUE;
    typedef HRESULT(WINAPI * DwmSet)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        if (auto set = (DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute")) set(g.hwnd, 20, &dark, sizeof(dark));

    tray_add();
    refresh_slow();
    SetTimer(g.hwnd, kTimerSlow, 1000, nullptr);
    if (!start_hidden) show_panel();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CloseHandle(single);
    return 0;
}
