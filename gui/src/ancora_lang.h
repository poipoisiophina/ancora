// ancora_lang.h
// Panel text from XML language files. One file per language: the root element
// <language code= name= dir= font=> holds one child element per string, named by
// its key (the ANCORA_STRINGS list below). Placeholders are {name} and are filled
// by sub(); no printf format ever comes from a file. Language files are embedded
// in ancora.exe as RCDATA resources by build_driver.bat; a file with the same code
// in the "lang" folder next to ancora.exe replaces the embedded one, and new codes
// there are added. A string a language lacks is shown in English.
#pragma once
#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "xmllite")
#pragma comment(lib, "shlwapi")

#define ANCORA_STRINGS(X) \
    X(nav_status) X(nav_tune) X(nav_diag) \
    X(rail_steamvr_on) X(rail_steamvr_off) X(rail_both) X(rail_one) X(rail_none) \
    X(lang_auto) X(lang_missing) \
    X(hint_back) X(hint_apply) X(hint_select) X(hint_next) \
    X(hand_left) X(hand_right) \
    X(state_no_signal) X(sub_no_signal) X(state_not_connected) X(sub_wake) X(sub_silent) \
    X(state_not_corrected) X(sub_unchanged) X(state_anchored) X(sub_drifting) X(sub_thumb_off) \
    X(state_in_deadzone) X(sub_in_deadzone) X(state_following) X(sub_past_deadzone) X(sub_as_is) \
    X(read_reports) X(read_games) X(read_thumb) \
    X(thumb_no_sensor) X(thumb_on) X(thumb_off) \
    X(set_gate) X(set_smoothing) X(set_deadzone) X(value_off) X(value_ms) X(value_percent) \
    X(master_on) X(master_off) \
    X(tune_suggested) X(legend_rests) X(legend_suggested) \
    X(step_rest) X(step_rest_active) X(step_rest_done) \
    X(step_flick) X(step_flick_settle) X(step_flick_release) X(step_flick_push) X(step_flick_done) \
    X(step_review) \
    X(res_peak) X(res_typical) X(res_direction) \
    X(dir_none) X(dir_right) X(dir_forward_right) X(dir_forward) X(dir_forward_left) \
    X(dir_left) X(dir_back_left) X(dir_back) X(dir_back_right) \
    X(find_none) X(find_gate_ok) X(find_gate_fail) X(find_no_sensor) X(find_creep) \
    X(tune_intro) X(btn_start) X(btn_apply_left) X(btn_apply_right) X(btn_restart) \
    X(edge_intro) X(btn_edge) X(btn_edge_apply_left) X(btn_edge_apply_right) \
    X(btn_edge_clear_left) X(btn_edge_clear_right) X(step_edge) X(step_edge_active) X(step_edge_done) \
    X(res_reach) X(find_edge_ok) X(find_edge_short) X(legend_reach) X(legend_edge_done) X(legend_edge_more) \
    X(set_edge) X(edge_not_measured) X(btn_edge_finish) X(edge_gap_hint) \
    X(tile_driver) X(drv_loaded) X(drv_registered) X(drv_missing) X(drv_patched) X(drv_not_seen) \
    X(tile_logic) X(logic_gen) X(logic_none) X(logic_loaded) \
    X(tile_config) X(cfg_pending) X(cfg_applied) \
    X(col_controller) X(col_inputs) X(col_thumb) X(col_updates) \
    X(diag_left_untracked) X(diag_right_untracked) X(inputs_silent) X(rate_idle) \
    X(log_title) X(log_empty) \
    X(btn_copy) X(btn_open_cfg) X(btn_open_log) \
    X(note_saved) X(what_anchoring) X(what_left_gate) X(what_right_gate) \
    X(what_left_smoothing) X(what_right_smoothing) X(what_left_deadzone) X(what_right_deadzone) \
    X(what_left_edge) X(what_right_edge) \
    X(what_language) X(note_copied) X(note_no_log) \
    X(tray_open) X(tray_exit)

enum Str {
#define ANCORA_ENUM(k) S_##k,
    ANCORA_STRINGS(ANCORA_ENUM)
#undef ANCORA_ENUM
    S_COUNT
};

static const char* const kStrKey[S_COUNT] = {
#define ANCORA_NAME(k) #k,
    ANCORA_STRINGS(ANCORA_NAME)
#undef ANCORA_NAME
};

struct LangInfo {
    std::wstring code, name, font = L"Segoe UI";
    bool rtl = false;
    std::wstring file;          // lang folder file, or empty for an embedded resource
    HRSRC res = nullptr;        // embedded resource
};

struct Lang {
    LangInfo info;
    std::wstring s[S_COUNT];
    std::vector<std::wstring> unknown;   // elements that are not a known key
    std::wstring error;                  // parse error, empty when the file read cleanly
    int missing() const { int n = 0; for (auto& v : s) n += v.empty(); return n; }
};

const size_t kMaxLangBytes = 256 * 1024;

inline std::string lang_bytes(const LangInfo& li) {
    if (li.res) {
        HGLOBAL h = LoadResource(nullptr, li.res);
        DWORD n = SizeofResource(nullptr, li.res);
        const char* p = h ? (const char*)LockResource(h) : nullptr;
        if (!p || n > kMaxLangBytes) return {};
        return std::string(p, n);
    }
    HANDLE h = CreateFileW(li.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string out;
    if (size.QuadPart > 0 && size.QuadPart <= (LONGLONG)kMaxLangBytes) {
        out.resize((size_t)size.QuadPart);
        DWORD rd = 0;
        ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
        out.resize(rd);
    }
    CloseHandle(h);
    return out;
}

// runs of whitespace that contain a line break become one space, ends trimmed,
// so a translator can wrap long strings over several lines in the file
inline std::wstring lang_clean(const std::wstring& in) {
    std::wstring out;
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == L' ' || in[i] == L'\t' || in[i] == L'\r' || in[i] == L'\n') {
            size_t j = i;
            bool br = false;
            while (j < in.size() && (in[j] == L' ' || in[j] == L'\t' || in[j] == L'\r' || in[j] == L'\n'))
                br |= in[j] == L'\r' || in[j] == L'\n', ++j;
            out += br ? std::wstring(L" ") : in.substr(i, j - i);
            i = j;
        } else {
            out += in[i++];
        }
    }
    size_t a = out.find_first_not_of(L' '), b = out.find_last_not_of(L' ');
    return a == std::wstring::npos ? std::wstring() : out.substr(a, b - a + 1);
}

// parses one language file; strings == nullptr reads the root attributes only
inline bool lang_parse(const std::string& bytes, LangInfo& info, Lang* strings) {
    if (bytes.empty()) { if (strings) strings->error = L"empty or too large"; return false; }
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    IXmlReader* r = nullptr;
    if (!st || FAILED(CreateXmlReader(__uuidof(IXmlReader), (void**)&r, nullptr))) {
        if (st) st->Release();
        return false;
    }
    r->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    r->SetInput(st);
    bool root = false, ok = true;
    int key = -1, open = 0;
    std::wstring value, unknown_name;
    XmlNodeType type;
    HRESULT hr;
    while ((hr = r->Read(&type)) == S_OK) {
        // own element depth: XmlLite reports an end tag one level below its start tag
        if (type == XmlNodeType_EndElement) --open;
        UINT depth = (UINT)open;
        if (type == XmlNodeType_Element) {
            const wchar_t* name = nullptr;
            r->GetLocalName(&name, nullptr);
            bool empty = r->IsEmptyElement() != FALSE;
            if (!empty) ++open;
            if (depth == 0) {
                if (wcscmp(name, L"language") != 0) { ok = false; break; }
                root = true;
                for (HRESULT a = r->MoveToFirstAttribute(); a == S_OK; a = r->MoveToNextAttribute()) {
                    const wchar_t *an = nullptr, *av = nullptr;
                    r->GetLocalName(&an, nullptr);
                    r->GetValue(&av, nullptr);
                    std::wstring v = av ? av : L"";
                    if (v.size() > 64) v.resize(64);
                    if (!wcscmp(an, L"code")) info.code = v;
                    else if (!wcscmp(an, L"name")) info.name = v;
                    else if (!wcscmp(an, L"font") && !v.empty()) info.font = v;
                    else if (!wcscmp(an, L"dir")) info.rtl = v == L"rtl";
                }
                if (!strings) break;
            } else if (depth == 1) {
                key = -2;
                std::string n8;
                for (const wchar_t* p = name; *p; ++p) n8 += (*p < 128) ? (char)*p : '?';
                for (int k = 0; k < S_COUNT; ++k)
                    if (n8 == kStrKey[k]) key = k;
                if (key == -2) unknown_name = name;
                value.clear();
                if (empty) { if (key == -2) strings->unknown.push_back(unknown_name); key = -1; }
            }
        } else if ((type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace) &&
                   key != -1 && strings) {
            const wchar_t* v = nullptr;
            UINT len = 0;
            r->GetValue(&v, &len);
            value.append(v, len);
        } else if (type == XmlNodeType_EndElement && depth == 1 && strings) {
            if (key >= 0) strings->s[key] = lang_clean(value);
            if (key == -2) strings->unknown.push_back(unknown_name);
            key = -1;
        }
    }
    if (FAILED(hr)) {
        ok = false;
        if (strings) {
            UINT line = 0;
            r->GetLineNumber(&line);
            strings->error = L"XML error on line " + std::to_wstring(line);
        }
    }
    r->Release();
    st->Release();
    if (info.name.empty()) info.name = info.code;
    return ok && root && !info.code.empty();
}

inline bool lang_code_ok(const std::wstring& c) {
    if (c.empty() || c.size() > 16) return false;
    for (wchar_t ch : c)
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-'))
            return false;
    return true;
}

inline BOOL CALLBACK lang_res_cb(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto* list = (std::vector<LangInfo>*)param;
    LangInfo li;
    li.res = FindResourceW(nullptr, name, RT_RCDATA);
    if (li.res && lang_parse(lang_bytes(li), li, nullptr) && lang_code_ok(li.code)) list->push_back(li);
    return TRUE;
}

// embedded languages, then the lang folder next to ancora.exe (same code replaces)
inline std::vector<LangInfo> lang_list() {
    std::vector<LangInfo> list;
    EnumResourceNamesW(nullptr, RT_RCDATA, lang_res_cb, (LONG_PTR)&list);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1) + L"lang\\";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"*.xml").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            LangInfo li;
            li.file = dir + fd.cFileName;
            if (!lang_parse(lang_bytes(li), li, nullptr) || !lang_code_ok(li.code)) continue;
            auto same = [&](const LangInfo& o) { return _wcsicmp(o.code.c_str(), li.code.c_str()) == 0; };
            list.erase(std::remove_if(list.begin(), list.end(), same), list.end());
            list.push_back(li);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(list.begin(), list.end(), [](const LangInfo& a, const LangInfo& b) { return a.code < b.code; });
    return list;
}

inline std::wstring lang_primary(const std::wstring& c) { return c.substr(0, c.find(L'-')); }

// a Windows language name (en-US, zh-TW, pt-PT, nb-NO) to one of the available codes
inline std::wstring lang_match(const std::vector<LangInfo>& list, std::wstring want) {
    for (auto& li : list) if (_wcsicmp(li.code.c_str(), want.c_str()) == 0) return li.code;
    std::wstring p = lang_primary(want);
    if (_wcsicmp(p.c_str(), L"zh") == 0) {
        bool trad = want.find(L"Hant") != std::wstring::npos || want.find(L"-TW") != std::wstring::npos ||
                    want.find(L"-HK") != std::wstring::npos || want.find(L"-MO") != std::wstring::npos;
        want = trad ? L"zh-Hant" : L"zh-Hans";
        for (auto& li : list) if (_wcsicmp(li.code.c_str(), want.c_str()) == 0) return li.code;
    }
    if (_wcsicmp(p.c_str(), L"no") == 0 || _wcsicmp(p.c_str(), L"nn") == 0) p = L"nb";
    for (auto& li : list) if (_wcsicmp(lang_primary(li.code).c_str(), p.c_str()) == 0) return li.code;
    return L"";
}

// "auto": the first Windows display language there is a file for, else English
inline std::wstring lang_resolve(const std::vector<LangInfo>& list, const std::wstring& setting) {
    if (!setting.empty() && _wcsicmp(setting.c_str(), L"auto") != 0) {
        if (setting.rfind(L"qps-", 0) == 0) return setting;   // pseudo languages for layout tests
        std::wstring m = lang_match(list, setting);
        if (!m.empty()) return m;
    }
    ULONG n = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, nullptr, &size) && size) {
        std::vector<wchar_t> buf(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, buf.data(), &size))
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
                std::wstring m = lang_match(list, p);
                if (!m.empty()) return m;
            }
    }
    return L"en";
}

// placeholders in a string, in order, such as {value} {n}
inline std::vector<std::wstring> lang_placeholders(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (size_t a = s.find(L'{'); a != std::wstring::npos; a = s.find(L'{', a + 1)) {
        size_t b = s.find(L'}', a);
        if (b == std::wstring::npos) break;
        out.push_back(s.substr(a, b - a + 1));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// pseudo languages built from English: accented and 40 % longer (qps-ploc), and
// the same laid out right to left (qps-plocm); they catch clipped and unmirrored text
inline void lang_pseudo(Lang& out, const Lang& en, bool mirrored) {
    static const wchar_t* from = L"aceinousyACEINOUSY";
    static const wchar_t* to   = L"\u00e5\u00e7\u00e9\u00ee\u00f1\u00f6\u00fb\u0161\u00fd\u00c5\u00c7\u00c9\u00ce\u00d1\u00d6\u00db\u0160\u00dd";
    out.info.code = mirrored ? L"qps-plocm" : L"qps-ploc";
    out.info.name = mirrored ? L"Pseudo mirrored" : L"Pseudo";
    out.info.rtl = mirrored;
    for (int k = 0; k < S_COUNT; ++k) {
        std::wstring s;
        bool in_ph = false;
        for (wchar_t c : en.s[k]) {
            if (c == L'{') in_ph = true;
            const wchar_t* p = in_ph ? nullptr : wcschr(from, c);
            s += p && c ? to[p - from] : c;
            if (c == L'}') in_ph = false;
        }
        size_t pad = (en.s[k].size() * 4 + 9) / 10;
        out.s[k] = L"[" + s + L" " + std::wstring(pad, L'~') + L"]";
    }
}
