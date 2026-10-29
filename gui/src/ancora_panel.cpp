// ancora_panel.cpp
// ancora control panel: Win32 + Direct2D (software target), no web runtime.
// Three pages in a SteamOS style layout: Status (both controllers, per hand
// settings), Tune (measures drift from the live stick and suggests a deadzone)
// and Diagnostics. Edits ancora.ini (the driver re-reads it within 250 ms) and
// reads live stick state the logic dll publishes in shared memory. Redraws only
// when something changed, at most 30 times a second, and draws nothing while
// hidden in the tray. Keyboard: Tab or arrows move focus, Enter or Space
// selects, Left/Right adjust a focused slider, Esc goes back.
//
// Usage:
//   ancora.exe                    open the panel
//   ancora.exe --tray             start hidden in the tray
//   ancora.exe --steamvr          started by SteamVR: tray, exit when SteamVR exits
//   ancora.exe --screenshot f.png [--fake] [--page status|tune|diag]   render one frame, exit
//   ancora.exe --icon-png f.png   write the 16/32/48/256 px icons side by side, exit
//   ancora.exe --icon-ico f.ico   write the app icon file that the build embeds, exit
//   ancora.exe --selftest         check ini read/write and the tune math, exit code 0 = pass

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
#include "../../driver/src/ancora_state.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "shell32")
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "ole32")

namespace {

// ---- palette (ancora brand) ---------------------------------------------------
D2D1_COLOR_F rgb(uint32_t c, float a = 1.0f) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a);
}
const uint32_t kBg = 0x0E141B, kNav = 0x0A0F14, kCard = 0x16202B, kSel = 0x1E2A36, kWell = 0x0E141B;
const uint32_t kGrid = 0x2A3644, kLine = 0x22303D;
const uint32_t kText = 0xE6EDF3, kMuted = 0xA9B6C2, kDim = 0x8B98A5, kRaw = 0xFF6B5A, kRawText = 0xFF8A7C;
const uint32_t kOut = 0x3DDC97, kOkWell = 0x12241D, kBrass = 0xF5C451, kWarn = 0xFF9F43, kWhite = 0xFFFFFF;

const float kW = 1280, kH = 610;            // layout size in DIPs
const float kNavW = 260, kFootH = 56;
const int kTrail = 120;                     // raw trail points per stick
const uint64_t kTrailMs = 2000;
const UINT WM_TRAY = WM_APP + 1;
const UINT kTimerFrame = 1, kTimerSlow = 2;
const UINT kFrameMs = 33;                   // at most 30 redraws a second

// ---- config (ancora.ini) ---------------------------------------------------------
// ini keys per hand: <hand>, <hand>_touch_gate, <hand>_deadzone
const wchar_t* kHandKey[2] = {L"left", L"right"};
const wchar_t* kHandName[2] = {L"Left controller", L"Right controller"};

struct HandConfig {
    bool  enable = true;
    bool  touch_gate = true;
    float deadzone = 0.0f;
};

struct Config {
    bool enable = true;
    HandConfig hand[2] = {{true, true, 0.0f}, {false, true, 0.0f}};
    bool exists = false;
};

std::wstring g_ini;
const Config kDefaults{};

std::wstring hand_key(int i, const wchar_t* suffix) { return std::wstring(kHandKey[i]) + suffix; }

Config read_config(const std::wstring& path) {
    Config c;
    c.exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const wchar_t* f = path.c_str();
    c.enable = GetPrivateProfileIntW(L"ancora", L"enable", 0, f) != 0;
    for (int i = 0; i < 2; ++i) {
        HandConfig& h = c.hand[i];
        h.enable     = GetPrivateProfileIntW(L"ancora", kHandKey[i], i == 0 ? 1 : 0, f) != 0;
        h.touch_gate = GetPrivateProfileIntW(L"ancora", hand_key(i, L"_touch_gate").c_str(), 1, f) != 0;
        wchar_t buf[32]{};
        GetPrivateProfileStringW(L"ancora", hand_key(i, L"_deadzone").c_str(), L"0", buf, 32, f);
        h.deadzone = (float)_wtof(buf);
        if (!(h.deadzone >= 0.0f)) h.deadzone = 0.0f;
        if (h.deadzone > 0.95f) h.deadzone = 0.95f;
    }
    return c;
}

void write_key(const std::wstring& path, const std::wstring& key, const std::wstring& value) {
    WritePrivateProfileStringW(L"ancora", key.c_str(), value.c_str(), path.c_str());
}

void write_bool(const std::wstring& path, const std::wstring& key, bool v) { write_key(path, key, v ? L"1" : L"0"); }

void write_deadzone(const std::wstring& path, int hand, float v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%.2f", v);
    write_key(path, hand_key(hand, L"_deadzone"), buf);
}

// full file with comments, used when the file is missing
void write_config_file(const std::wstring& path, const Config& c) {
    wchar_t text[1024];
    swprintf(text, 1024,
             L"; ancora driver config. The driver re-reads it within 250 ms while SteamVR runs.\r\n"
             L"[ancora]\r\n"
             L"; master switch, 0 = pass all input through unchanged\r\n"
             L"enable=%d\r\n"
             L"; per hand: correct the stick (1 = on), thumb gate (thumb off the cap -> 0,0),\r\n"
             L"; radial deadzone while touched (0..0.95, 0 = off)\r\n"
             L"left=%d\r\n"
             L"left_touch_gate=%d\r\n"
             L"left_deadzone=%.2f\r\n"
             L"right=%d\r\n"
             L"right_touch_gate=%d\r\n"
             L"right_deadzone=%.2f\r\n",
             c.enable, c.hand[0].enable, c.hand[0].touch_gate, c.hand[0].deadzone,
             c.hand[1].enable, c.hand[1].touch_gate, c.hand[1].deadzone);
    std::string utf8;
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    utf8.resize(n - 1);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), n, nullptr, nullptr);
    // write to a temp file and swap, so the driver never reads half a file
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
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
    std::string j = read_file(local_appdata() + L"\\openvr\\openvrpaths.vrpath");
    size_t k = j.find("\"log\"");
    if (k != std::string::npos) {
        size_t a = j.find('"', j.find('[', k) + 1);
        size_t b = j.find('"', a + 1);
        if (a != std::string::npos && b != std::string::npos) {
            std::string p = j.substr(a + 1, b - a - 1);
            std::string clean;
            for (size_t i = 0; i < p.size(); ++i) {   // JSON escapes a backslash as two
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                clean += p[i];
            }
            return widen(clean) + L"\\vrserver.txt";
        }
    }
    return L"";
}

bool driver_registered() {
    std::string j = read_file(local_appdata() + L"\\openvr\\openvrpaths.vrpath");
    return j.find("ancora\\\\driver\\\\ancora") != std::string::npos;
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
    r.enable = 1; r.touch_gate = 0; r.deadzone = 0.20f;
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
    const wchar_t* direction = L"none";
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
        static const wchar_t* names[8] = {L"right", L"forward right", L"forward", L"forward left",
                                          L"left", L"back left", L"back", L"back right"};
        int k = (int)std::lround((a < 0 ? a + 360 : a) / 45.0f) % 8;
        r.direction = names[k];
    }
    return r;
}

enum TuneStage { TUNE_IDLE, TUNE_REST, TUNE_FLICK, TUNE_REVIEW };
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
};

Tune g_tune;

void tune_reset(int hand) {
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
    if (g_tune.stage != TUNE_REST && g_tune.stage != TUNE_FLICK) return false;
    if (!live_now(now)) return false;
    const AncoraHandState& h = g_live.snap.hand[g_tune.hand];
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
                      *f_small = nullptr, *f_label = nullptr, *f_mono = nullptr, *f_mono_big = nullptr, *f_nav = nullptr;
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
    int drag_hand = -1;             // deadzone slider being dragged
    D2D1_RECT_F drag_track{};
    bool exiting = false, from_steamvr = false;
    NOTIFYICONDATAW nid{};
};

App g;

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
enum Id { ID_NONE, ID_NAV_STATUS, ID_NAV_TUNE, ID_NAV_DIAG, ID_MASTER, ID_CORRECT, ID_GATE, ID_SLIDER,
          ID_TUNE_HAND, ID_TUNE_START, ID_TUNE_APPLY, ID_TUNE_RESET, ID_COPY, ID_OPEN_CFG, ID_OPEN_LOG };
struct Region { D2D1_RECT_F r; Id id; int hand; };
std::vector<Region> g_regions;     // rebuilt every frame, in focus order
Id g_focus = ID_NAV_STATUS;
int g_focus_hand = 0;
bool g_focus_visible = false;      // keyboard focus ring only after a key press

bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
bool focused(Id id, int hand = 0) { return g_focus_visible && g_focus == id && g_focus_hand == hand; }
void region(D2D1_RECT_F r, Id id, int hand = 0) { g_regions.push_back({r, id, hand}); }

// ---- drawing helpers -----------------------------------------------------------------
IDWriteTextFormat* make_format(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family = L"Segoe UI") {
    IDWriteTextFormat* f = nullptr;
    g.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

void text(ID2D1RenderTarget* t, const std::wstring& s, IDWriteTextFormat* f, float x, float y, uint32_t c,
          float w = 800, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    f->SetTextAlignment(al);
    g.br->SetColor(rgb(c));
    t->DrawText(s.c_str(), (UINT32)s.size(), f, D2D1::RectF(x, y, x + w, y + 60), g.br);
}

// wrapped paragraph inside width w
void para(ID2D1RenderTarget* t, const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, uint32_t c) {
    IDWriteTextLayout* lay = nullptr;
    g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, 200, &lay);
    lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.br->SetColor(rgb(c));
    t->DrawTextLayout(D2D1::Point2F(x, y), lay, g.br);
    lay->Release();
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

void draw_switch(ID2D1RenderTarget* t, float x, float y, bool on) {
    D2D1_RECT_F r = {x, y, x + 46, y + 26};
    fill_round(t, r, 13, on ? kOut : kGrid);
    circle(t, on ? x + 33 : x + 13, y + 13, 10, on ? kBg : kDim);
}

void draw_button(ID2D1RenderTarget* t, D2D1_RECT_F r, const std::wstring& s, bool primary, Id id, int hand = 0) {
    fill_round(t, r, 12, primary ? kOut : kCard);
    text(t, s, g.f_h2, r.left, r.top + (r.bottom - r.top - 24) / 2, primary ? kBg : kText, r.right - r.left,
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

    const wchar_t* names[3] = {L"Status", L"Tune", L"Diagnostics"};
    Id ids[3] = {ID_NAV_STATUS, ID_NAV_TUNE, ID_NAV_DIAG};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = {16, 98 + i * 62.0f, kNavW - 16, 154 + i * 62.0f};
        bool sel = g.page == (Page)i;
        if (sel) fill_round(t, r, 12, kSel);
        nav_icon(t, i, r.left + 16, r.top + 17, sel ? kWhite : kMuted);
        text(t, names[i], g.f_nav, r.left + 52, r.top + 15, sel ? kWhite : kMuted);
        if (focused(ids[i])) ring(t, r, 12);
        region(r, ids[i]);
    }

    // live summary at the bottom of the rail
    D2D1_RECT_F box = {16, kH - 104, kNavW - 16, kH - 20};
    fill_round(t, box, 12, 0x121821);
    bool live = live_now(now);
    int found = 0;
    for (int i = 0; i < 2; ++i) found += live && connected(g_live.snap.hand[i], now);
    circle(t, box.left + 20, box.top + 25, 4, g.steamvr ? kOut : kDim);
    text(t, g.steamvr ? L"SteamVR running" : L"SteamVR off", g.f_small, box.left + 32, box.top + 15, kMuted);
    circle(t, box.left + 20, box.top + 57, 4, found == 2 ? kOut : found ? kBrass : kDim);
    text(t, found == 2 ? L"Both controllers" : found == 1 ? L"One controller" : L"No controllers", g.f_small,
         box.left + 32, box.top + 47, kMuted);
}

void key_hint(ID2D1RenderTarget* t, float& x, float y, const wchar_t* key, const wchar_t* what) {
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
    key_hint(t, x, y, L"Esc", L"Back");
    if (g.page == PAGE_TUNE && g_tune.stage == TUNE_REVIEW) key_hint(t, x, y, L"Enter", L"Apply");
    else key_hint(t, x, y, L"Enter", L"Select");
    key_hint(t, x, y, L"Tab", L"Next");
    if (GetTickCount64() < g.note_until) text(t, g.status_note, g.f_body, kNavW + 32, y + 4, kOut, 400);
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
    if (!live) return {L"No signal", L"Start SteamVR to see this controller.", kDim};
    if ((h.tracked & 3) != 3 || !h.last_update_ms) return {L"Not connected", L"Wake the controller to see its stick.", kWarn};
    if (!connected(h, now))
        return {L"Not connected", fmt1(L"No stick data for %.0f s. Check its battery or wake it.",
                                       (now - h.last_update_ms) / 1000.0), kWarn};
    if (!s.enable || !h.enable) return {L"Not corrected", L"Passed to games unchanged.", kMuted};
    if (h.touch_gate && (h.tracked & 4) && !h.touch)
        return {L"Drift anchored", m > 0.02f ? fmt1(L"Drifting %.2f. Thumb off, so games get 0.", m)
                                             : std::wstring(L"Thumb off, so games get 0."), kOut};
    if (h.deadzone > 0 && m < h.deadzone)
        return {L"In deadzone", fmt1(L"Inside the %.0f%% deadzone, so games get 0.", h.deadzone * 100), kOut};
    return {L"Following thumb", h.deadzone > 0 ? fmt1(L"Past the %.0f%% deadzone, games get a scaled push.", h.deadzone * 100)
                                               : std::wstring(L"Passed to games as is."), kBrass};
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

void draw_hand_card(ID2D1RenderTarget* t, int i, uint64_t now) {
    float x0 = 292 + i * 486.0f;
    D2D1_RECT_F c = {x0, 84, x0 + 470, kH - kFootH - 24};
    fill_round(t, c, 18, kCard);
    const AncoraHandState& h = g_live.snap.hand[i];
    const HandConfig& hc = g.cfg.hand[i];
    bool live = live_now(now), tracked = live && connected(h, now);

    text(t, kHandName[i], g.f_h2, x0 + 22, c.top + 18, kText);
    D2D1_RECT_F sw = {x0 + 470 - 22 - 46, c.top + 18, x0 + 470 - 22, c.top + 44};
    draw_switch(t, sw.left, sw.top, hc.enable);
    if (focused(ID_CORRECT, i)) ring(t, sw, 13);
    region(sw, ID_CORRECT, i);

    // stick
    float R = 82, cx = x0 + 22 + R, cy = c.top + 62 + R;
    draw_target(t, cx, cy, R, (live && h.enable && g_live.snap.enable) ? h.deadzone : 0, false);
    draw_trail(t, i, cx, cy, R, now);
    if (tracked) {
        circle(t, cx + h.raw_x * R, cy - h.raw_y * R, 7, kRaw, 1, false, 2);
        circle(t, cx + h.out_x * R, cy - h.out_y * R, 15, kOut, 0.18f);
        circle(t, cx + h.out_x * R, cy - h.out_y * R, 8, kOut);
    }

    HandView v = describe_hand(i, now);
    text(t, v.word, g.f_big, x0 + 210, c.top + 92, v.color, 250);
    para(t, v.sub, g.f_body, x0 + 210, c.top + 134, 240, kMuted);

    // readouts
    float ry = c.top + 246, bw = (470 - 44 - 16) / 3.0f;
    const wchar_t* lab[3] = {L"Reports", L"Games get", L"Thumb"};
    for (int k = 0; k < 3; ++k) {
        D2D1_RECT_F b = {x0 + 22 + k * (bw + 8), ry, x0 + 22 + k * (bw + 8) + bw, ry + 54};
        fill_round(t, b, 10, kWell);
        text(t, lab[k], g.f_small, b.left + 12, b.top + 7, kDim);
        if (!tracked) { text(t, L"--", g.f_mono, b.left + 12, b.top + 26, kDim); continue; }
        if (k == 0) text(t, fmt2(L"%+.2f %+.2f", h.raw_x, h.raw_y), g.f_mono, b.left + 12, b.top + 26, kRawText);
        if (k == 1) text(t, fmt2(L"%+.2f %+.2f", h.out_x, h.out_y), g.f_mono, b.left + 12, b.top + 26, kOut);
        if (k == 2) text(t, !(h.tracked & 4) ? L"no sensor" : h.touch ? L"on stick" : L"off", g.f_body, b.left + 12,
                         b.top + 25, h.touch ? kBrass : kMuted);
    }

    // per hand settings
    D2D1_RECT_F well = {x0 + 22, ry + 70, x0 + 470 - 22, ry + 70 + 104};
    fill_round(t, well, 12, kWell);
    D2D1_RECT_F gate = {well.left, well.top, well.right, well.top + 52};
    text(t, L"Thumb gate", g.f_body, gate.left + 14, gate.top + 15, kText);
    draw_switch(t, gate.right - 14 - 46, gate.top + 13, hc.touch_gate);
    if (focused(ID_GATE, i)) ring(t, gate, 12);
    region(gate, ID_GATE, i);
    line(t, well.left, well.top + 52, well.right, well.top + 52, kLine);

    D2D1_RECT_F dzr = {well.left, well.top + 52, well.right, well.bottom};
    text(t, L"Deadzone", g.f_body, dzr.left + 14, dzr.top + 15, kText);
    std::wstring val = hc.deadzone > 0 ? fmt1(L"%.0f%%", hc.deadzone * 100) : std::wstring(L"off");
    text(t, val, g.f_mono, dzr.right - 64, dzr.top + 16, hc.deadzone > 0 ? kText : kMuted, 50,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    D2D1_RECT_F track = {dzr.left + 110, dzr.top + 26, dzr.right - 78, dzr.top + 26};
    line(t, track.left, track.top, track.right, track.top, kGrid, 4);
    float kx = track.left + (track.right - track.left) * (hc.deadzone / 0.95f);
    line(t, track.left, track.top, kx, track.top, kOut, 4);
    circle(t, kx, track.top, 9, kText);
    if (focused(ID_SLIDER, i)) ring(t, dzr, 12);
    region({track.left - 12, dzr.top, track.right + 12, dzr.bottom}, ID_SLIDER, i);
}

void draw_status(ID2D1RenderTarget* t, uint64_t now) {
    text(t, L"Status", g.f_title, 292, 24, kText);
    // master switch pill
    std::wstring label = g.cfg.enable ? L"Anchoring on" : L"Anchoring off";
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
void draw_step(ID2D1RenderTarget* t, D2D1_RECT_F r, int n, int state, const wchar_t* title, const std::wstring& sub,
               float progress) {
    // state: 0 waiting, 1 active, 2 done
    fill_round(t, r, 14, state == 1 ? kSel : kCard);
    float cx = r.left + 30, cy = r.top + 28;
    if (state == 2) {
        circle(t, cx, cy, 14, kOkWell);
        line(t, cx - 6, cy, cx - 2, cy + 4, kOut, 2.2f);
        line(t, cx - 2, cy + 4, cx + 6, cy - 4, kOut, 2.2f);
    } else if (state == 1) {
        circle(t, cx, cy, 14, kOut);
        text(t, std::to_wstring(n), g.f_label, cx - 14, cy - 9, kBg, 28, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        circle(t, cx, cy, 13, 0x3A4552, 1, false, 1.5f);
        text(t, std::to_wstring(n), g.f_label, cx - 14, cy - 9, kDim, 28, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    text(t, title, g.f_h2, r.left + 58, r.top + (sub.empty() ? 16 : 8), state == 0 ? kMuted : kText);
    if (!sub.empty()) text(t, sub, g.f_small, r.left + 58, r.top + 32, kMuted, r.right - r.left - 70);
    if (state == 1 && progress >= 0) {
        D2D1_RECT_F bar = {r.right - 150, r.top + 25, r.right - 20, r.top + 31};
        fill_round(t, bar, 3, kBg);
        fill_round(t, {bar.left, bar.top, bar.left + (bar.right - bar.left) * progress, bar.bottom}, 3, kOut);
    }
}

void draw_tune(ID2D1RenderTarget* t, uint64_t now) {
    text(t, L"Tune", g.f_title, 292, 24, kText);
    // hand selector
    D2D1_RECT_F seg = {kW - 32 - 320, 22, kW - 32, 66};
    fill_round(t, seg, 12, kCard);
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F b = {seg.left + 4 + i * 156.0f, seg.top + 4, seg.left + 160 + i * 156.0f, seg.bottom - 4};
        bool sel = g_tune.hand == i;
        if (sel) fill_round(t, b, 9, kGrid);
        text(t, kHandName[i], g.f_body, b.left, b.top + 9, sel ? kWhite : kMuted, b.right - b.left,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        if (focused(ID_TUNE_HAND, i)) ring(t, b, 9);
        region(b, ID_TUNE_HAND, i);
    }

    // measurement target
    D2D1_RECT_F left = {292, 84, 672, kH - kFootH - 24};
    fill_round(t, left, 18, kCard);
    float R = 150, cx = (left.left + left.right) / 2, cy = (left.top + left.bottom - 40) / 2;
    bool review = g_tune.stage == TUNE_REVIEW;
    draw_target(t, cx, cy, R, review ? g_tune.result.suggested : 0, true);
    for (const Point& p : g_tune.pts) circle(t, cx + p.x * R, cy - p.y * R, 3, kRaw, 0.8f);
    const AncoraHandState& h = g_live.snap.hand[g_tune.hand];
    if (live_now(now) && connected(h, now) && !review)
        circle(t, cx + h.raw_x * R, cy - h.raw_y * R, 7, kRaw, 1, false, 2);
    if (review && g_tune.result.suggested > 0)
        text(t, fmt1(L"suggested %.2f", g_tune.result.suggested), g.f_small, cx - 100,
             cy + R * g_tune.result.suggested + 6, kOut, 200, DWRITE_TEXT_ALIGNMENT_CENTER);
    float ly = left.bottom - 34;
    circle(t, left.left + 60, ly + 9, 4.5f, kRaw);
    text(t, L"Where it rests", g.f_small, left.left + 72, ly, kMuted);
    line(t, left.left + 200, ly + 9, left.left + 214, ly + 9, kOut, 2);
    text(t, L"Suggested deadzone", g.f_small, left.left + 222, ly, kMuted);

    // steps
    float x0 = 692, x1 = kW - 32, y = 84;
    int st = g_tune.stage;
    float rest_p = st == TUNE_REST ? std::fmin(1.0f, (now - g_tune.t0) / (float)kRestMs) : -1;
    draw_step(t, {x0, y, x1, y + 56}, 1, st == TUNE_REST ? 1 : st > TUNE_REST ? 2 : 0, L"Let go of the stick",
              st == TUNE_REST ? L"Recording where it rests, keep your thumb off." :
              st > TUNE_REST ? L"5 s of rest recorded." : L"", rest_p);
    y += 64;
    draw_step(t, {x0, y, x1, y + 56}, 2, st == TUNE_FLICK ? 1 : st > TUNE_FLICK ? 2 : 0, L"Flick it and let go",
              st == TUNE_FLICK ? (g_tune.armed && g_tune.release_t ? fmt1(L"%.0f of 6. Let go, measuring where it settles.", g_tune.flicks)
                                  : g_tune.armed ? fmt1(L"%.0f of 6. Now let go.", g_tune.flicks)
                                  : fmt1(L"%.0f of 6. Push it all the way, then let go.", g_tune.flicks)) :
              st > TUNE_FLICK ? L"6 of 6 flicks recorded." : L"",
              st == TUNE_FLICK ? g_tune.flicks / (float)kFlicks : -1);
    y += 64;
    draw_step(t, {x0, y, x1, y + 56}, 3, review ? 1 : 0, L"Review and apply", L"", -1);
    y += 72;

    // results and findings
    if (review) {
        const TuneResult& r = g_tune.result;
        D2D1_RECT_F box = {x0, y, x1, y + 168};
        fill_round(t, box, 14, kCard);
        float cw = (x1 - x0 - 32) / 3;
        text(t, fmt1(L"%.2f", r.peak), g.f_mono_big, x0 + 16, y + 12, kRawText);
        text(t, L"peak drift", g.f_small, x0 + 16, y + 46, kMuted);
        text(t, fmt1(L"%.2f", r.typical), g.f_mono_big, x0 + 16 + cw, y + 12, kText);
        text(t, L"typical drift", g.f_small, x0 + 16 + cw, y + 46, kMuted);
        text(t, r.direction, g.f_big, x0 + 16 + 2 * cw, y + 12, kText, cw);
        text(t, L"direction", g.f_small, x0 + 16 + 2 * cw, y + 46, kMuted);
        float fy = y + 76;
        if (r.peak < 0.03f) {
            text(t, L"\u2713", g.f_h2, x0 + 16, fy, kOut);
            para(t, L"Barely any drift measured. No deadzone needed.", g.f_body, x0 + 40, fy + 2, x1 - x0 - 56, kText);
        } else {
            text(t, r.gate_ok ? L"\u2713" : L"!", g.f_h2, x0 + 16, fy, r.gate_ok ? kOut : kBrass);
            para(t, r.gate_ok ? L"Thumb sensor stayed off while you let go, so the thumb gate already stops this drift."
                              : r.has_sensor ? L"Thumb sensor read on while you let go, so the thumb gate alone cannot stop it."
                                             : L"No thumb sensor found, so only a deadzone can stop it.",
                 g.f_body, x0 + 40, fy + 2, x1 - x0 - 56, kText);
            text(t, L"!", g.f_h2, x0 + 16, fy + 46, kBrass);
            para(t, fmt1(L"With your thumb resting on the stick it can still creep. A %.2f deadzone covers that.",
                         r.suggested), g.f_body, x0 + 40, fy + 48, x1 - x0 - 56, kText);
        }
    } else if (st == TUNE_IDLE) {
        para(t, L"Measures how this stick drifts and suggests a deadzone. Takes about 20 seconds: let go of the stick, "
                L"then flick it a few times.", g.f_body, x0 + 4, y, x1 - x0 - 8, kMuted);
    }

    // actions
    float by = kH - kFootH - 24 - 48, bx = x1;
    auto button_right = [&](const std::wstring& s, bool primary, Id id) {
        float w = measure(s, g.f_h2) + 44;
        draw_button(t, {bx - w, by, bx, by + 48}, s, primary, id);
        bx -= w + 10;
    };
    if (st == TUNE_IDLE) button_right(L"Start measuring", true, ID_TUNE_START);
    else if (review && g_tune.result.peak >= 0.03f)
        button_right(fmt1(L"Apply %.2f deadzone to ", g_tune.result.suggested) + (g_tune.hand ? L"right" : L"left"),
                     true, ID_TUNE_APPLY);
    if (st != TUNE_IDLE) button_right(L"Start over", false, ID_TUNE_RESET);
}

// ---- diagnostics page ---------------------------------------------------------------------
void draw_diag(ID2D1RenderTarget* t, uint64_t now) {
    text(t, L"Diagnostics", g.f_title, 292, 24, kText);
    float x0 = 292, x1 = kW - 32, y = 84, tw = (x1 - x0 - 24) / 3;
    const LogFacts& lf = g.log;
    struct Tile { const wchar_t* head; std::wstring main, sub; };
    Tile tiles[3] = {
        {L"Driver", g.registered ? (lf.tables > 0 ? L"Loaded" : L"Registered") : L"Not installed",
         lf.tables > 0 ? fmt1(L"%.0f input tables patched", lf.tables) : std::wstring(L"not seen in the SteamVR log")},
        {L"Logic", lf.generation > 0 ? fmt1(L"Generation %.0f", lf.generation) : std::wstring(L"Not loaded"),
         lf.generation > 0 ? L"loaded " + lf.gen_time + fmt1(L", %.0f inputs replayed", lf.replayed) : std::wstring(L"")},
        {L"Config", lf.config_time.empty() ? std::wstring(L"Not applied yet") : L"Applied " + lf.config_time,
         L"%LOCALAPPDATA%\\ancora\\ancora.ini"}};
    for (int k = 0; k < 3; ++k) {
        D2D1_RECT_F b = {x0 + k * (tw + 12), y, x0 + k * (tw + 12) + tw, y + 80};
        fill_round(t, b, 14, kCard);
        text(t, tiles[k].head, g.f_small, b.left + 16, b.top + 12, kMuted);
        text(t, tiles[k].main, g.f_h2, b.left + 16, b.top + 30, kText, tw - 24);
        text(t, tiles[k].sub, g.f_small, b.left + 16, b.top + 56, kMuted, tw - 24);
    }
    y += 96;

    // controller table
    D2D1_RECT_F tb = {x0, y, x1, y + 136};
    fill_round(t, tb, 14, kCard);
    float cols[6] = {x0 + 16, x0 + 260, x0 + 400, x0 + 510, x0 + 660, x1 - 110};
    const wchar_t* heads[6] = {L"Controller", L"Inputs found", L"Thumb now", L"Reports", L"Games get", L"Updates"};
    for (int k = 0; k < 6; ++k) text(t, heads[k], g.f_small, cols[k], y + 12, kMuted, k == 5 ? 94.0f : 200.0f,
                                     k == 5 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    bool live = live_now(now);
    for (int i = 0; i < 2; ++i) {
        float ry = y + 40 + i * 46;
        line(t, x0, ry, x1, ry, kLine);
        const AncoraHandState& h = g_live.snap.hand[i];
        bool tracked = live && connected(h, now);
        text(t, live && h.model[0] ? widen(h.model) : std::wstring(i ? L"Right: not tracked" : L"Left: not tracked"),
             g.f_body, cols[0], ry + 13, live && h.model[0] ? kText : kDim, 236);
        std::wstring inputs = live ? std::wstring(h.tracked & 1 ? L"x " : L"") + (h.tracked & 2 ? L"y " : L"") +
                                         (h.tracked & 4 ? L"touch" : L"")
                                   : L"--";
        if (live && (h.tracked & 3) == 3 && !tracked) { inputs = L"silent"; }
        text(t, inputs, g.f_mono, cols[1], ry + 14, tracked ? kOut : live && (h.tracked & 3) == 3 ? kWarn : kDim);
        text(t, !tracked ? L"--" : !(h.tracked & 4) ? L"no sensor" : h.touch ? L"on stick" : L"off", g.f_body, cols[2],
             ry + 13, tracked && h.touch ? kBrass : kMuted);
        text(t, tracked ? fmt2(L"%+.2f %+.2f", h.raw_x, h.raw_y) : L"--", g.f_mono, cols[3], ry + 14, kRawText);
        text(t, tracked ? fmt2(L"%+.2f %+.2f", h.out_x, h.out_y) : L"--", g.f_mono, cols[4], ry + 14, kOut);
        std::wstring rate = !live ? L"--" : g_live.rate[i] < 1 ? L"idle" : fmt1(L"%.0f/s", g_live.rate[i]);
        text(t, rate, g.f_mono, cols[5], ry + 14, kText, 94, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    y += 152;

    // log tail
    D2D1_RECT_F lb = {x0, y, x1, kH - kFootH - 24 - 64};
    fill_round(t, lb, 14, kNav);
    text(t, L"SteamVR log, ancora lines", g.f_small, lb.left + 16, lb.top + 10, kMuted);
    size_t n = lf.lines.size(), rows = (size_t)((lb.bottom - lb.top - 36) / 19), first = n > rows ? n - rows : 0;
    t->PushAxisAlignedClip(D2D1::RectF(lb.left + 8, lb.top + 30, lb.right - 8, lb.bottom - 6), D2D1_ANTIALIAS_MODE_ALIASED);
    float ly = lb.top + 32;
    for (size_t k = first; k < n; ++k, ly += 19) text(t, lf.lines[k], g.f_mono, lb.left + 16, ly, kMuted, 3000);
    if (!n) text(t, L"No ancora lines in the SteamVR log yet.", g.f_body, lb.left + 16, ly, kDim);
    t->PopAxisAlignedClip();

    // actions
    float by = kH - kFootH - 24 - 48, bx = x0;
    auto button_left = [&](const std::wstring& s, Id id) {
        float w = measure(s, g.f_h2) + 44;
        draw_button(t, {bx, by, bx + w, by + 48}, s, false, id);
        bx += w + 10;
    };
    button_left(L"Copy diagnostics", ID_COPY);
    button_left(L"Open config file", ID_OPEN_CFG);
    button_left(L"Open SteamVR log", ID_OPEN_LOG);
}

void draw(ID2D1RenderTarget* t) {
    uint64_t now = g_fake ? g_live.snap.tick_ms : GetTickCount64();
    g_regions.clear();
    t->Clear(rgb(kBg));
    draw_nav(t, now);
    if (g.page == PAGE_STATUS) draw_status(t, now);
    if (g.page == PAGE_TUNE) draw_tune(t, now);
    if (g.page == PAGE_DIAG) draw_diag(t, now);
    draw_footer(t);
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
void ensure_file() {
    if (GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES) write_config_file(g_ini, g.cfg);
}

void saved(const std::wstring& what) {
    g.ini_time = file_time(g_ini);
    note(what + L" saved");
}

void set_deadzone(int hand, float v) {
    v = std::round(std::fmax(0.0f, std::fmin(0.95f, v)) * 100) / 100;
    g.cfg.hand[hand].deadzone = v;
}

void commit_deadzone(int hand) {
    ensure_file();
    write_deadzone(g_ini, hand, g.cfg.hand[hand].deadzone);
    saved(hand ? L"Right deadzone" : L"Left deadzone");
}

void copy_diagnostics() {
    std::wstring d = L"ancora diagnostics\r\nconfig: " + g_ini + L"\r\nlog: " + g.log_path + L"\r\n";
    d += g.steamvr ? L"SteamVR running\r\n" : L"SteamVR off\r\n";
    d += g.registered ? L"driver registered\r\n" : L"driver not registered\r\n";
    for (int i = 0; i < 2; ++i) {
        const AncoraHandState& h = g_live.snap.hand[i];
        d += std::wstring(kHandKey[i]) + L": " + widen(h.model) +
             fmt2(L" tracked=%.0f touch=%.0f", h.tracked, h.touch) + L"\r\n";
    }
    for (auto& l : g.log.lines) d += l + L"\r\n";
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (d.size() + 1) * sizeof(wchar_t));
        std::memcpy(GlobalLock(mem), d.c_str(), (d.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
        note(L"Diagnostics copied");
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
        saved(L"Anchoring");
        break;
    case ID_CORRECT:
        ensure_file();
        g.cfg.hand[hand].enable = !g.cfg.hand[hand].enable;
        write_bool(g_ini, kHandKey[hand], g.cfg.hand[hand].enable);
        saved(hand ? L"Right controller" : L"Left controller");
        break;
    case ID_GATE:
        ensure_file();
        g.cfg.hand[hand].touch_gate = !g.cfg.hand[hand].touch_gate;
        write_bool(g_ini, hand_key(hand, L"_touch_gate"), g.cfg.hand[hand].touch_gate);
        saved(hand ? L"Right thumb gate" : L"Left thumb gate");
        break;
    case ID_TUNE_HAND:  if (g_tune.hand != hand) tune_reset(hand); break;
    case ID_TUNE_START: tune_start(); break;
    case ID_TUNE_RESET: tune_reset(g_tune.hand); break;
    case ID_TUNE_APPLY:
        set_deadzone(g_tune.hand, g_tune.result.suggested);
        commit_deadzone(g_tune.hand);
        break;
    case ID_COPY:     copy_diagnostics(); break;
    case ID_OPEN_CFG: ensure_file(); ShellExecuteW(g.hwnd, L"open", L"notepad.exe", g_ini.c_str(), nullptr, SW_SHOW); break;
    case ID_OPEN_LOG:
        if (g.log_path.empty()) note(L"SteamVR log location unknown");
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
    if (g_tune.stage == TUNE_REST || g_tune.stage == TUNE_FLICK) tune_reset(g_tune.hand);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

void redraw() { InvalidateRect(g.hwnd, nullptr, FALSE); }

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
        if (g.drag_hand >= 0) {
            set_deadzone(g.drag_hand, (x - g.drag_track.left) / (g.drag_track.right - g.drag_track.left) * 0.95f);
            redraw();
            return 0;
        }
        SetCursor(LoadCursor(nullptr, hit_at(x, y) ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / ui_scale(), y = GET_Y_LPARAM(lp) / ui_scale();
        const Region* r = hit_at(x, y);
        trace("down %.0f,%.0f id=%d hand=%d", x, y, r ? (int)r->id : 0, r ? r->hand : -1);
        g_focus_visible = false;
        if (r) {
            g_focus = r->id;
            g_focus_hand = r->hand;
            if (r->id == ID_SLIDER) {
                g.drag_hand = r->hand;
                g.drag_track = {r->r.left + 12, r->r.top, r->r.right - 12, r->r.bottom};
                SetCapture(h);
                set_deadzone(r->hand, (x - g.drag_track.left) / (g.drag_track.right - g.drag_track.left) * 0.95f);
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
            commit_deadzone(hand);
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
            if (g_focus == ID_SLIDER) {
                set_deadzone(g_focus_hand, g.cfg.hand[g_focus_hand].deadzone + (wp == VK_RIGHT ? 0.05f : -0.05f));
                commit_deadzone(g_focus_hand);
            } else {
                move_focus(wp == VK_RIGHT ? 1 : -1);
            }
        } else if (wp == VK_RETURN || wp == VK_SPACE) {
            if (g_focus != ID_SLIDER) activate(g_focus, g_focus_hand);
        } else if (wp == VK_ESCAPE) {
            if (g.page != PAGE_STATUS) { g.page = PAGE_STATUS; g_focus = ID_NAV_STATUS; g_focus_hand = 0; }
            else hide_panel();
        }
        redraw();
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) { IsWindowVisible(h) ? hide_panel() : show_panel(); }
        if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Open ancora");
            AppendMenuW(menu, MF_STRING, 2, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) show_panel();
            if (cmd == 2) { g.exiting = true; DestroyWindow(h); }
        }
        return 0;
    case WM_CLOSE:
        if (wp == 1 || g.exiting) { DestroyWindow(h); return 0; }
        hide_panel();   // close button keeps ancora in the tray
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void create_resources() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g.dw);
    float dashes[2] = {3, 3};
    g.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                             dashes, 2, &g.dash);
    g.f_title    = make_format(28, DWRITE_FONT_WEIGHT_BOLD);
    g.f_logo     = make_format(24, DWRITE_FONT_WEIGHT_BOLD);
    g.f_nav      = make_format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.f_h2       = make_format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.f_big      = make_format(26, DWRITE_FONT_WEIGHT_BOLD);
    g.f_body     = make_format(15, DWRITE_FONT_WEIGHT_NORMAL);
    g.f_small    = make_format(13, DWRITE_FONT_WEIGHT_NORMAL);
    g.f_label    = make_format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.f_mono     = make_format(14, DWRITE_FONT_WEIGHT_NORMAL, L"Consolas");
    g.f_mono_big = make_format(26, DWRITE_FONT_WEIGHT_NORMAL, L"Consolas");
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
int screenshot(const std::wstring& out, const std::wstring& page) {
    create_resources();
    g.cfg = read_config(g_ini);
    g.page = page == L"tune" ? PAGE_TUNE : page == L"diag" ? PAGE_DIAG : PAGE_STATUS;
    if (g_fake) {
        g.cfg.enable = true; g.cfg.hand[0] = {true, true, 0.0f}; g.cfg.hand[1] = {true, false, 0.20f};
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
        note(L"Right deadzone saved");
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
    std::wprintf(L"screenshot %ls (%ux%u) draw hr=0x%08lx\n", out.c_str(), W, H, (unsigned long)hr);
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
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

    Config c = read_config(p);
    check(!c.exists && !c.enable && c.hand[0].enable && !c.hand[1].enable && c.hand[0].touch_gate &&
          c.hand[1].touch_gate && c.hand[0].deadzone == 0 && c.hand[1].deadzone == 0,
          "missing file reads safe defaults (disabled)");
    Config d; d.enable = true; d.hand[1] = {true, false, 0.2f};
    write_config_file(p, d);
    c = read_config(p);
    check(c.exists && c.enable && c.hand[1].enable && !c.hand[1].touch_gate && std::fabs(c.hand[1].deadzone - 0.2f) < 1e-4 &&
          c.hand[0].touch_gate && c.hand[0].deadzone == 0, "full file write and read back, hands independent");
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
    check(std::wcscmp(r.direction, L"forward") == 0, "tune direction of forward drift is forward");
    check(r.gate_ok, "tune: thumb never on while let go -> gate alone stops it");
    check(!tune_compute(pts, 30, 100, true).gate_ok, "tune: thumb on 30 % of the time -> gate not enough");
    check(!tune_compute(pts, 0, 100, false).gate_ok, "tune: no touch sensor -> gate not enough");
    check(tune_compute({{0.0f, 0.95f}}, 0, 10, true).suggested <= 0.95f, "tune suggestion never above 0.95");

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
    check(g_tune.result.peak > 0.3f && g_tune.result.peak < 0.5f && std::wcscmp(g_tune.result.direction, L"forward") == 0,
          "tune result on the drifting stick: peak about 0.4, forward");
    check(g_tune.result.gate_ok, "tune: thumb was off after every release -> gate alone stops it");
    tune_reset(0);
    std::printf("%d failure(s)\n", fails);
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
    if (has(L"--icon-png")) return icon_sheet(arg_after(L"--icon-png", L"ancora_icon.png"));
    if (has(L"--icon-ico")) return write_ico(arg_after(L"--icon-ico", L"ancora.ico"));
    if (has(L"--screenshot")) {
        attach_console();
        g_fake = has(L"--fake") != 0;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return screenshot(arg_after(L"--screenshot", L"ancora.png"), arg_after(L"--page", L"status"));
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
