// ancora_pad_read.h
// Finds and reads gamepads for the logic dll, inside vrserver, and for the panel.
// Neither way needs a window or focus, so this works while the game is in the
// background (a game that reads the pad itself only gets input while its window
// is in front).
//   XInput: Xbox pads, and pads Steam Input or DS4Windows present as Xbox pads
//   HID: every other gamepad, wired or Bluetooth, read from its own input
//   reports. PS4 and PS5 pads by their report layout (ancora_pad.h), since
//   their full Bluetooth reports are not in their HID descriptor; any other pad
//   through its descriptor: left stick X/Y, right stick Z/Rz (else Rx/Ry)
// No DirectInput or winmm joystick calls: those run DirectInput inside the
// process, which crashed the panel.
// An Xbox pad shows up on both; its HID twin (Microsoft's vendor id) is left out
// while any XInput pad is there. scan() looks for pads and runs once a second,
// poll() reads the pads in use. The logic runs both on its pad thread, never under
// its lock. The panel runs the same code to see which pad a button is pressed
// on: HID pads are listed in device path order, so its ids are the driver's.
// Names come from the pads themselves: the HID product string, for an XInput pad
// the one of the HID device with its vendor and product id (XInput gives those
// through its undocumented XInputGetCapabilitiesEx), else the name in Windows'
// game controller settings, else the two ids.

#pragma once
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "ancora_pad.h"

#pragma comment(lib, "advapi32")
#pragma comment(lib, "setupapi")
#pragma comment(lib, "hid")

struct PadDevice {
    char id[kPadIdLen] = {};
    char name[kPadNameLen] = {};
    bool xinput = false;
    UINT slot = 0;                  // XInput slot
    int  hid = -1;                  // index into PadReader::hid_
    WORD vid = 0, pid = 0;          // 0, 0 = not known (an XInput pad on an old XInput)
    PadReading last;
};

class PadReader {
public:
    PadDevice dev[kPadMax];
    int count = 0;

    PadReader() = default;
    PadReader(const PadReader&) = delete;
    PadReader& operator=(const PadReader&) = delete;
    ~PadReader() { close(); }

    // lets go of every pad; the next scan() opens them again
    void close() {
        for (auto& p : hid_) if (p) close_hid(*p);
        hid_.clear();
        for (int k = 0; k < kPadMax; ++k) dev[k] = PadDevice{};
        count = 0;
    }

    // lists every pad found now; true when the list or a name changed
    bool scan() {
        refresh_hid();
        PadDevice found[kPadMax];
        int n = 0;
        for (DWORD i = 0; i < 4 && n < kPadMax; ++i) {
            PadDevice& d = found[n];
            if (!read_xinput(i, d.last)) continue;
            d.xinput = true;
            d.slot = i;
            std::snprintf(d.id, sizeof(d.id), "xinput:%u", (unsigned)i);
            xinput_ids(i, d.vid, d.pid);
            name_xinput(d);
            ++n;
        }
        bool any_xinput = n > 0;
        for (size_t k = 0; k < hid_.size() && n < kPadMax; ++k) {
            HidPad& p = *hid_[k];
            if (any_xinput && p.vid == 0x045E) continue;   // an Xbox pad's twin
            drain(p);
            if (p.dead) continue;
            PadDevice& d = found[n];
            d.hid = (int)k;
            d.vid = p.vid;
            d.pid = p.pid;
            d.last = p.last;
            char base[kPadIdLen];
            std::snprintf(base, sizeof(base), "hid:%04x:%04x", d.vid, d.pid);
            int same = 1;
            for (int j = 0; j < n; ++j) if (std::strncmp(found[j].id, base, std::strlen(base)) == 0) ++same;
            if (same == 1) std::snprintf(d.id, sizeof(d.id), "%s", base);
            else std::snprintf(d.id, sizeof(d.id), "%s#%d", base, same > 9 ? 9 : same);
            std::memcpy(d.name, p.name, sizeof(d.name));
            ++n;
        }
        bool changed = n != count;
        for (int k = 0; k < n && !changed; ++k)
            changed = std::strcmp(found[k].id, dev[k].id) != 0 || std::strcmp(found[k].name, dev[k].name) != 0;
        for (int k = 0; k < n; ++k) dev[k] = found[k];
        for (int k = n; k < kPadMax; ++k) dev[k] = PadDevice{};
        count = n;
        return changed;
    }

    // reads pad k again; a pad that stopped answering reads as not connected
    void poll(int k) {
        if (k < 0 || k >= count) return;
        PadDevice& d = dev[k];
        if (d.xinput) {
            PadReading r;
            if (!read_xinput(d.slot, r)) r = PadReading{};
            d.last = r;
            return;
        }
        if (d.hid < 0 || d.hid >= (int)hid_.size()) { d.last = PadReading{}; return; }
        HidPad& p = *hid_[d.hid];
        drain(p);
        d.last = p.dead ? PadReading{} : p.last;
    }

private:
    // ---- XInput --------------------------------------------------------------------
    struct XState {   // XINPUT_STATE, without the header (it differs between SDKs)
        DWORD packet;
        WORD buttons;
        BYTE lt, rt;
        SHORT lx, ly, rx, ry;
    };
    struct XCapsEx {  // XINPUT_CAPABILITIES_EX of xinput1_4.dll
        BYTE type, subtype;
        WORD flags;
        WORD buttons;
        BYTE lt, rt;
        SHORT lx, ly, rx, ry;
        WORD rumble_l, rumble_r;
        WORD vid, pid, version, unknown1;
        DWORD unknown2;
    };
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
    using XInputGetCapsExFn = DWORD(WINAPI*)(DWORD, DWORD, DWORD, XCapsEx*);
    XInputGetStateFn xget_ = nullptr;
    XInputGetCapsExFn xcaps_ = nullptr;
    bool x_loaded_ = false;

    void load_xinput() {
        if (x_loaded_) return;
        x_loaded_ = true;
        const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"};
        for (const wchar_t* d : dlls) {
            HMODULE m = LoadLibraryExW(d, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!m || !(xget_ = (XInputGetStateFn)GetProcAddress(m, "XInputGetState"))) continue;
            xcaps_ = (XInputGetCapsExFn)GetProcAddress(m, MAKEINTRESOURCEA(108));   // only in xinput1_4
            break;
        }
    }

    bool read_xinput(DWORD slot, PadReading& r) {
        load_xinput();
        XState s{};
        if (!xget_ || xget_(slot, &s) != ERROR_SUCCESS) return false;
        r.connected = true;
        r.lx = pad_xinput_axis(s.lx); r.ly = pad_xinput_axis(s.ly);
        r.rx = pad_xinput_axis(s.rx); r.ry = pad_xinput_axis(s.ry);
        r.buttons = s.buttons | (s.lt > 128 ? 1u << 16 : 0u) | (s.rt > 128 ? 1u << 17 : 0u);
        return true;
    }

    void xinput_ids(DWORD slot, WORD& vid, WORD& pid) {
        XCapsEx c{};
        if (xcaps_ && xcaps_(1, slot, 0, &c) == ERROR_SUCCESS) { vid = c.vid; pid = c.pid; }
    }

    // ---- HID gamepads, kept open between scans ---------------------------------------
    struct Axis { bool on = false; USAGE usage = 0; LONG lo = 0, hi = 0; USHORT bits = 0; };
    struct HidPad {
        std::wstring path;
        HANDLE h = INVALID_HANDLE_VALUE;
        HANDLE ev = nullptr;
        OVERLAPPED ov{};
        bool pending = false, dead = false;
        PHIDP_PREPARSED_DATA pp = nullptr;
        USHORT report_len = 0;
        WORD vid = 0, pid = 0;
        int sony = 0;               // pad_sony_model
        Axis ax[4];                 // lx, ly, rx, ry for a pad read through its descriptor
        unsigned char buf[512] = {};
        char name[kPadNameLen] = {};
        PadReading last;
    };
    std::vector<std::unique_ptr<HidPad>> hid_;
    std::vector<std::wstring> not_pads_;   // HID devices seen that are no gamepad

    static void close_hid(HidPad& p) {
        if (p.h != INVALID_HANDLE_VALUE) {
            // the read in flight writes into buf: it must be over before buf goes
            if (p.pending) {
                DWORD n;
                CancelIoEx(p.h, &p.ov);
                GetOverlappedResult(p.h, &p.ov, &n, TRUE);
            }
            CloseHandle(p.h);
        }
        if (p.ev) CloseHandle(p.ev);
        if (p.pp) HidD_FreePreparsedData(p.pp);
        p.h = INVALID_HANDLE_VALUE; p.ev = nullptr; p.pp = nullptr; p.pending = false;
    }

    // every HID interface present now, in path order
    static std::vector<std::wstring> hid_paths() {
        std::vector<std::wstring> v;
        GUID g;
        HidD_GetHidGuid(&g);
        HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return v;
        SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
        alignas(8) BYTE buf[4096];
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &ifd); ++i) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
            if (!need || need > sizeof(buf)) continue;
            auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) v.push_back(det->DevicePath);
        }
        SetupDiDestroyDeviceInfoList(set);
        std::sort(v.begin(), v.end());
        return v;
    }

    // opens a HID device; false when it is no gamepad (*not_pad) or cannot be opened now
    static bool open_hid(const std::wstring& path, HidPad& p, bool* not_pad) {
        *not_pad = false;
        p.path = path;
        p.h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (p.h == INVALID_HANDLE_VALUE)
            p.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
        if (p.h == INVALID_HANDLE_VALUE) {
            // keyboards and mice are opened by Windows alone: never a pad
            *not_pad = GetLastError() == ERROR_ACCESS_DENIED;
            return false;
        }
        HIDP_CAPS caps{};
        if (!HidD_GetPreparsedData(p.h, &p.pp) || HidP_GetCaps(p.pp, &caps) != HIDP_STATUS_SUCCESS ||
            caps.UsagePage != 0x01 || (caps.Usage != 0x04 && caps.Usage != 0x05) || caps.InputReportByteLength < 2 ||
            caps.InputReportByteLength > sizeof(p.buf)) {
            *not_pad = true;
            close_hid(p);
            return false;
        }
        p.report_len = caps.InputReportByteLength;
        HIDD_ATTRIBUTES a{sizeof(a)};
        if (HidD_GetAttributes(p.h, &a)) { p.vid = a.VendorID; p.pid = a.ProductID; }
        p.sony = pad_sony_model(p.vid, p.pid);
        setup_axes(p, caps);
        wchar_t w[kPadNameLen]{};
        if (!(HidD_GetProductString(p.h, w, sizeof(w) - sizeof(wchar_t)) && to_utf8(w, p.name, sizeof(p.name))) &&
            !oem_name(p.vid, p.pid, p.name, sizeof(p.name)))
            std::snprintf(p.name, sizeof(p.name), "%04x:%04x", p.vid, p.pid);
        p.ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        p.ov.hEvent = p.ev;
        if (!p.ev) { close_hid(p); return false; }
        return true;
    }

    static void setup_axes(HidPad& p, const HIDP_CAPS& caps) {
        std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps ? caps.NumberInputValueCaps : 1);
        USHORT n = caps.NumberInputValueCaps;
        if (!n || HidP_GetValueCaps(HidP_Input, vc.data(), &n, p.pp) != HIDP_STATUS_SUCCESS) return;
        Axis found[6];   // X, Y, Z, Rx, Ry, Rz
        for (USHORT i = 0; i < n; ++i) {
            const HIDP_VALUE_CAPS& c = vc[i];
            if (c.UsagePage != 0x01 || c.IsRange) continue;
            USAGE u = c.NotRange.Usage;
            if (u < 0x30 || u > 0x35) continue;
            Axis& a = found[u - 0x30];
            a.on = true;
            a.usage = u;
            a.bits = c.BitSize;
            a.lo = c.LogicalMin;
            a.hi = c.LogicalMax;
            if (a.hi <= a.lo && a.bits > 0 && a.bits < 32) { a.lo = 0; a.hi = (LONG)((1u << a.bits) - 1); }
        }
        bool z_rz = found[2].on && found[5].on;
        p.ax[0] = found[0];
        p.ax[1] = found[1];
        p.ax[2] = z_rz ? found[2] : found[3];
        p.ax[3] = z_rz ? found[5] : found[4];
    }

    static float signed_axis(LONG v, LONG lo, LONG hi) {
        if (hi <= lo) return 0.0f;
        float a = ((float)v - (float)lo) / (float)(hi - lo) * 2.0f - 1.0f;
        return a < -1.0f ? -1.0f : a > 1.0f ? 1.0f : a;
    }

    // one input report of n bytes into p.last
    static void parse(HidPad& p, ULONG n) {
        if (n == 0) return;
        PadReading r = p.last;
        if (p.sony && pad_parse_sony(p.sony, p.buf, (int)n, p.report_len, r)) { p.last = r; return; }
        bool any = false;
        float* out[4] = {&r.lx, &r.ly, &r.rx, &r.ry};
        for (int k = 0; k < 4; ++k) {
            const Axis& a = p.ax[k];
            ULONG v = 0;
            if (!a.on || HidP_GetUsageValue(HidP_Input, 0x01, 0, a.usage, &v, p.pp, (PCHAR)p.buf, n) != HIDP_STATUS_SUCCESS)
                continue;
            LONG s = (LONG)v;
            if (a.lo < 0 && a.bits > 0 && a.bits < 32 && (v & (1u << (a.bits - 1)))) s = (LONG)(v | (~0u << a.bits));
            float f = signed_axis(s, a.lo, a.hi);
            *out[k] = k & 1 ? -f : f;   // HID y grows downward
            any = true;
        }
        USAGE used[64];
        ULONG nu = 64;
        if (HidP_GetUsages(HidP_Input, 0x09, 0, used, &nu, p.pp, (PCHAR)p.buf, n) == HIDP_STATUS_SUCCESS) {
            unsigned mask = 0;
            for (ULONG i = 0; i < nu; ++i) if (used[i] >= 1 && used[i] <= 32) mask |= 1u << (used[i] - 1);
            r.buttons = mask;
            any = true;
        }
        if (any) { r.connected = true; p.last = r; }
    }

    // takes every report waiting, keeps the newest; a failed read marks the pad gone
    static void drain(HidPad& p) {
        if (p.dead) return;
        for (int i = 0; i < 64; ++i) {
            if (!p.pending) {
                ResetEvent(p.ev);
                if (!ReadFile(p.h, p.buf, p.report_len, nullptr, &p.ov) && GetLastError() != ERROR_IO_PENDING) {
                    p.dead = true;
                    return;
                }
                p.pending = true;
            }
            DWORD n = 0;
            if (!GetOverlappedResult(p.h, &p.ov, &n, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return;
                p.pending = false;
                p.dead = true;
                return;
            }
            p.pending = false;
            parse(p, n);
        }
    }

    // opens gamepads that appeared, closes the ones that went away, in path order
    void refresh_hid() {
        std::vector<std::wstring> paths = hid_paths();
        std::vector<std::unique_ptr<HidPad>> next;
        for (const std::wstring& path : paths) {
            if (std::find(not_pads_.begin(), not_pads_.end(), path) != not_pads_.end()) continue;
            auto it = std::find_if(hid_.begin(), hid_.end(), [&](const std::unique_ptr<HidPad>& p) { return p && p->path == path; });
            if (it != hid_.end() && !(*it)->dead) { next.push_back(std::move(*it)); continue; }
            if (next.size() >= (size_t)kPadMax) continue;
            auto p = std::make_unique<HidPad>();
            bool not_pad = false;
            if (open_hid(path, *p, &not_pad)) next.push_back(std::move(p));
            else if (not_pad && not_pads_.size() < 512) not_pads_.push_back(path);
        }
        for (auto& p : hid_) if (p) close_hid(*p);
        hid_ = std::move(next);
    }

    // ---- names ---------------------------------------------------------------------
    static bool to_utf8(const wchar_t* w, char* out, int size) {
        out[0] = 0;
        // a name too long for the buffer in UTF-8 counts as none
        if (!w[0] || !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, nullptr, nullptr)) { out[0] = 0; return false; }
        out[size - 1] = 0;
        return true;
    }

    // the name Windows shows in its game controller settings (OEMName under the
    // pad's VID_xxxx&PID_xxxx key)
    static bool oem_name(WORD vid, WORD pid, char* out, int size) {
        wchar_t key[160], w[kPadNameLen]{};
        swprintf(key, 160, L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_%04X&PID_%04X",
                 vid, pid);
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            DWORD bytes = sizeof(w) - sizeof(wchar_t);
            if (RegGetValueW(root, key, L"OEMName", RRF_RT_REG_SZ, nullptr, w, &bytes) == ERROR_SUCCESS && to_utf8(w, out, size))
                return true;
        }
        return false;
    }

    // product string of the HID device with this vendor and product id, opened
    // for neither reading nor writing, which needs no access rights
    static bool hid_name(WORD vid, WORD pid, char* out, int size) {
        for (const std::wstring& path : hid_paths()) {
            HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            HIDD_ATTRIBUTES a{sizeof(a)};
            wchar_t w[kPadNameLen]{};
            bool got = HidD_GetAttributes(h, &a) && a.VendorID == vid && a.ProductID == pid &&
                       HidD_GetProductString(h, w, sizeof(w) - sizeof(wchar_t)) && to_utf8(w, out, size);
            CloseHandle(h);
            if (got) return true;
        }
        return false;
    }

    // names already looked up for XInput pads, by vendor and product id
    struct Known { WORD vid, pid; char name[kPadNameLen]; };
    Known known_[8] = {};
    int nknown_ = 0;

    void name_xinput(PadDevice& d) {
        if (!d.vid && !d.pid) { std::snprintf(d.name, sizeof(d.name), "%s", d.id); return; }   // only the slot is known
        for (int k = 0; k < nknown_; ++k)
            if (known_[k].vid == d.vid && known_[k].pid == d.pid) { std::memcpy(d.name, known_[k].name, sizeof(d.name)); return; }
        for (auto& p : hid_)
            if (p->vid == d.vid && p->pid == d.pid && p->name[0]) { std::memcpy(d.name, p->name, sizeof(d.name)); return; }
        bool got = hid_name(d.vid, d.pid, d.name, sizeof(d.name)) || oem_name(d.vid, d.pid, d.name, sizeof(d.name));
        if (!got) { std::snprintf(d.name, sizeof(d.name), "%04x:%04x", d.vid, d.pid); return; }
        if (nknown_ < 8) {
            known_[nknown_].vid = d.vid;
            known_[nknown_].pid = d.pid;
            std::memcpy(known_[nknown_].name, d.name, sizeof(d.name));
            ++nknown_;
        }
    }
};
