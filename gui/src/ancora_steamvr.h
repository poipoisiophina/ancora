// ancora_steamvr.h
// SteamVR side of an ancora install, run as ancora.exe --check, --register or
// --unregister by the installer, install.ps1 and uninstall.ps1. Locations come
// from SteamVR's own %LOCALAPPDATA%\openvr\openvrpaths.vrpath ("runtime" has
// vrpathreg.exe, "config" is Steam\config), never from fixed paths.
//   check       exit 1 while SteamVR runs (its loaded driver files are locked)
//   register    register <root>\driver\ancora with SteamVR and the panel as an
//               auto launch app
//   unregister  undo register and remove everything ancora left in Steam\config
// Steam files are backed up before a change to <backup dir>\<file>.<time>.bak,
// never into Steam\config. The JSON edits are plain functions on file paths, so
// the panel self test runs them on temp files.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace svr {

// ---- minimal JSON document: keeps key order, numbers as their source text ----------
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    std::string s;                                   // Num: source text, Str: UTF-8
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    Json* find(const std::string& k) {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool erase(const std::string& k) {
        for (size_t i = 0; i < o.size(); ++i)
            if (o[i].first == k) { o.erase(o.begin() + (std::ptrdiff_t)i); return true; }
        return false;
    }
    static Json str(const std::string& v) { Json j; j.type = Str; j.s = v; return j; }
};

struct JsonParser {
    const std::string& t;
    size_t p = 0;
    int depth = 0;
    explicit JsonParser(const std::string& text) : t(text) {
        if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) p = 3;
    }
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) ++p; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (t.compare(p, n, w) != 0) return false;
        p += n;
        return true;
    }
    static void put_utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (p + 4 > t.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p >= t.size() || t[p] != '"') return false;
        ++p;
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t c1;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && p + 1 < t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                    p += 2;
                    uint32_t c2;
                    if (!hex4(c2) || c2 < 0xDC00 || c2 > 0xDFFF) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                put_utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json& v) {
        if (++depth > 64) return false;
        ws();
        if (p >= t.size()) return false;
        bool ok = false;
        char c = t[p];
        if (c == '{') {
            v.type = Json::Obj; ++p; ws();
            if (p < t.size() && t[p] == '}') { ++p; ok = true; }
            else for (;;) {
                std::string k;
                ws();
                if (!string(k)) break;
                ws();
                if (p >= t.size() || t[p] != ':') break;
                ++p;
                Json item;
                if (!value(item)) break;
                v.o.emplace_back(std::move(k), std::move(item));
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; ok = true; }
                break;
            }
        } else if (c == '[') {
            v.type = Json::Arr; ++p; ws();
            if (p < t.size() && t[p] == ']') { ++p; ok = true; }
            else for (;;) {
                Json item;
                if (!value(item)) break;
                v.a.push_back(std::move(item));
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; ok = true; }
                break;
            }
        } else if (c == '"') {
            v.type = Json::Str;
            ok = string(v.s);
        } else if (lit("true")) { v.type = Json::Bool; v.b = true; ok = true; }
        else if (lit("false")) { v.type = Json::Bool; ok = true; }
        else if (lit("null")) { v.type = Json::Null; ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t s = p;
            if (t[p] == '-') ++p;
            while (p < t.size() && ((t[p] >= '0' && t[p] <= '9') || t[p] == '.' || t[p] == 'e' || t[p] == 'E' ||
                                    t[p] == '+' || t[p] == '-')) ++p;
            v.type = Json::Num;
            v.s = t.substr(s, p - s);
            ok = v.s != "-";
        }
        --depth;
        return ok;
    }
};

// whole text must be one JSON value; false leaves out unchanged
inline bool json_parse(const std::string& text, Json& out) {
    JsonParser jp(text);
    Json v;
    if (!jp.value(v)) return false;
    jp.ws();
    if (jp.p != text.size()) return false;
    out = std::move(v);
    return true;
}

inline void json_escape(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if ((unsigned char)c < 0x20) { char u[8]; std::snprintf(u, sizeof(u), "\\u%04x", (unsigned char)c); out += u; }
            else out += c;
        }
    }
    out += '"';
}

// written the way SteamVR writes its own files: 3 space indent, "key" : value
inline void json_write(std::string& out, const Json& v, int indent = 0) {
    std::string pad((size_t)(indent + 1) * 3, ' '), end((size_t)indent * 3, ' ');
    switch (v.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += v.b ? "true" : "false"; break;
    case Json::Num: out += v.s; break;
    case Json::Str: json_escape(out, v.s); break;
    case Json::Arr:
        if (v.a.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t i = 0; i < v.a.size(); ++i) {
            out += pad;
            json_write(out, v.a[i], indent + 1);
            out += i + 1 < v.a.size() ? ",\n" : "\n";
        }
        out += end + "]";
        break;
    case Json::Obj:
        if (v.o.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t i = 0; i < v.o.size(); ++i) {
            out += pad;
            json_escape(out, v.o[i].first);
            out += " : ";
            json_write(out, v.o[i].second, indent + 1);
            out += i + 1 < v.o.size() ? ",\n" : "\n";
        }
        out += end + "}";
        break;
    }
}

// ---- files and text -------------------------------------------------------------------
inline std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
inline bool is_dir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool read_all(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > (16 << 20)) { CloseHandle(h); return false; }
    out.assign((size_t)size.QuadPart, '\0');
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    out.resize(rd);
    return ok != FALSE;
}

// temp file then swap, so SteamVR never reads half a file
inline bool write_all(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

inline bool make_dirs(const std::wstring& dir) {
    if (dir.empty() || is_dir(dir)) return true;
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2 && !make_dirs(dir.substr(0, cut))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

inline bool remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

inline std::wstring leaf(const std::wstring& p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? p : p.substr(cut + 1);
}

// folder part of a path, "" when it has none
inline std::wstring leaf_dir(const std::wstring& p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

inline bool ends_with_nocase(const std::string& s, const char* tail) {
    size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i], b = tail[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// output goes to stdout: the console or pipe of whoever started ancora.exe
inline void say(const std::wstring& s) {
    std::string u = utf8(s + L"\n");
    std::fwrite(u.data(), 1, u.size(), stdout);
    std::fflush(stdout);
}

// copy to <backup_dir>\<file>.<yyyyMMdd-HHmmss>.bak; empty when the copy failed
inline std::wstring backup(const std::wstring& path, const std::wstring& backup_dir) {
    if (!make_dirs(backup_dir)) return L"";
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    // a second backup of the same file within one second gets -2, -3, ... instead of
    // replacing the first
    std::wstring base = backup_dir + L"\\" + leaf(path) + L"." + stamp, dest = base + L".bak";
    for (int n = 2; exists(dest) && n < 100; ++n) dest = base + L"-" + std::to_wstring(n) + L".bak";
    return CopyFileW(path.c_str(), dest.c_str(), TRUE) ? dest : L"";
}

// ---- the edits ------------------------------------------------------------------------
// entries ancora owns in Steam\config: steamvr.vrsettings sections and the matching
// vrappconfig\<name>.vrappconfig files (all but driver_ancora have one). SteamVR
// names an app without a manifest system.generated.<exe name>, so only exe names
// that are ancora's alone are listed: the diagnostic tools are ancora_probe.exe and
// ancora_reader.exe for that reason. Sections of 0.1.3 and older tools named plain
// probe.exe and reader.exe are left alone, since another program can have them.
inline const std::vector<std::string>& owned_sections() {
    static const std::vector<std::string> v = {"driver_ancora", "ancora.panel", "ancora.reader",
                                               "system.generated.ancora.exe", "system.generated.ancora_reader.exe",
                                               "system.generated.ancora_probe.exe"};
    return v;
}

enum class Edit { Unchanged, Written, Failed };

// rewrite a JSON file after a backup, only when edit() reports a change; a missing
// file is left alone, an unreadable one is reported and left alone
template <class F>
Edit edit_json(const std::wstring& path, const std::wstring& backup_dir, F edit) {
    std::string text;
    if (!exists(path)) return Edit::Unchanged;
    Json j;
    if (!read_all(path, text) || !json_parse(text, j)) { say(L"cannot read " + path + L", left as is"); return Edit::Failed; }
    if (!edit(j)) return Edit::Unchanged;
    std::wstring bak = backup(path, backup_dir);
    if (bak.empty()) { say(L"backup of " + path + L" failed, left as is"); return Edit::Failed; }
    std::string out;
    json_write(out, j);
    out += "\n";
    if (!write_all(path, out)) { say(L"cannot write " + path); return Edit::Failed; }
    say(L"updated " + path + L" (backup " + bak + L")");
    return Edit::Written;
}

// drops ancora's manifest from appconfig.json "manifest_paths"; adds `manifest` when given
inline bool appconfig_set(Json& j, const std::string& manifest) {
    if (j.type != Json::Obj) return false;
    Json* paths = j.find("manifest_paths");
    if (!paths) {
        if (manifest.empty()) return false;
        Json arr;
        arr.type = Json::Arr;
        j.o.emplace_back("manifest_paths", std::move(arr));
        paths = &j.o.back().second;
    }
    if (paths->type != Json::Arr) {
        if (manifest.empty()) return false;
        *paths = Json();
        paths->type = Json::Arr;
    }
    std::vector<Json> keep;
    for (auto& e : paths->a)
        if (!(e.type == Json::Str && ends_with_nocase(e.s, "ancora.vrmanifest"))) keep.push_back(e);
    if (!manifest.empty()) keep.push_back(Json::str(manifest));
    // already as wanted (a repeated register): no write, no backup
    bool changed = keep.size() != paths->a.size();
    for (size_t i = 0; !changed && i < keep.size(); ++i)
        changed = paths->a[i].type != keep[i].type || paths->a[i].s != keep[i].s;
    paths->a = std::move(keep);
    return changed;
}

inline bool vrsettings_strip(Json& j) {
    if (j.type != Json::Obj) return false;
    bool hit = false;
    for (auto& k : owned_sections()) hit |= j.erase(k);
    return hit;
}

// appconfig.json with ancora's manifest; created when missing
inline bool appconfig_register(const std::wstring& path, const std::wstring& manifest, const std::wstring& backup_dir) {
    if (!exists(path)) {
        Json j;
        j.type = Json::Obj;
        appconfig_set(j, utf8(manifest));
        std::string out;
        json_write(out, j);
        out += "\n";
        if (!write_all(path, out)) { say(L"cannot write " + path); return false; }
        say(L"created " + path);
        return true;
    }
    return edit_json(path, backup_dir, [&](Json& j) { return appconfig_set(j, utf8(manifest)); }) != Edit::Failed;
}

// ---- SteamVR ---------------------------------------------------------------------------
inline bool process_running(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

inline bool steamvr_running() { return process_running(L"vrserver.exe") || process_running(L"vrmonitor.exe"); }

struct Paths {
    std::wstring vrpathreg;   // empty when SteamVR is not set up
    std::wstring config;      // Steam\config, empty when not found
    std::wstring log;         // SteamVR's log folder, empty when not found
    std::vector<std::string> external_drivers;   // registered driver folders, UTF-8
    std::wstring vrpath;      // openvrpaths.vrpath itself
};

inline Paths find_paths() {
    Paths r;
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    r.vrpath = std::wstring(buf) + L"\\openvr\\openvrpaths.vrpath";
    std::string text;
    Json j;
    if (!read_all(r.vrpath, text) || !json_parse(text, j) || j.type != Json::Obj) return r;
    if (Json* rt = j.find("runtime"); rt && rt->type == Json::Arr)
        for (auto& e : rt->a) {
            if (e.type != Json::Str) continue;
            std::wstring exe = wide(e.s) + L"\\bin\\win64\\vrpathreg.exe";
            if (exists(exe)) { r.vrpathreg = exe; break; }
        }
    if (Json* cf = j.find("config"); cf && cf->type == Json::Arr)
        for (auto& e : cf->a)
            if (e.type == Json::Str && is_dir(wide(e.s))) { r.config = wide(e.s); break; }
    if (Json* lg = j.find("log"); lg && lg->type == Json::Arr)
        for (auto& e : lg->a)
            if (e.type == Json::Str) { r.log = wide(e.s); break; }
    if (Json* ed = j.find("external_drivers"); ed && ed->type == Json::Arr)
        for (auto& e : ed->a)
            if (e.type == Json::Str) r.external_drivers.push_back(e.s);
    return r;
}

// runs vrpathreg with its window hidden; its output goes to `out`; exit code, -1 = not started
inline int run_vrpathreg(const std::wstring& exe, const std::wstring& args, std::string* out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) if (out) out->append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

inline std::wstring quoted(const std::wstring& p) { return L"\"" + p + L"\""; }

inline int do_register(const std::wstring& root, const std::wstring& backup_dir) {
    Paths sp = find_paths();
    if (sp.vrpathreg.empty()) { say(L"SteamVR not found (" + sp.vrpath + L"). Run SteamVR once first."); return 1; }
    if (sp.config.empty()) { say(L"Steam config folder from openvrpaths.vrpath not found"); return 1; }
    std::wstring driver = root + L"\\driver\\ancora";
    std::string out;
    int code = run_vrpathreg(sp.vrpathreg, L"adddriver " + quoted(driver), &out);
    if (!out.empty()) say(wide(out));
    if (code != 0) { say(L"vrpathreg adddriver failed (" + std::to_wstring(code) + L")"); return 1; }
    say(L"registered driver " + driver);

    // panel: manifest path in appconfig.json, auto launch in vrappconfig
    std::wstring manifest = root + L"\\ancora.vrmanifest";
    if (!appconfig_register(sp.config + L"\\appconfig.json", manifest, backup_dir)) return 1;
    std::wstring app_dir = sp.config + L"\\vrappconfig";
    if (!make_dirs(app_dir) || !write_all(app_dir + L"\\ancora.panel.vrappconfig", "{ \"autolaunch\" : true }\n")) {
        say(L"cannot write " + app_dir + L"\\ancora.panel.vrappconfig");
        return 1;
    }
    say(L"registered panel " + manifest + L" (auto launch on)");
    return 0;
}

// missing pieces are skipped, so a half removed setup still cleans up
inline int do_unregister(const std::wstring& root, const std::wstring& backup_dir) {
    Paths sp = find_paths();
    int fails = 0;
    if (!sp.vrpathreg.empty()) {
        run_vrpathreg(sp.vrpathreg, L"removedriver " + quoted(root + L"\\driver\\ancora"), nullptr);
        std::string shown;
        run_vrpathreg(sp.vrpathreg, L"show", &shown);
        std::string left;
        size_t a = 0;
        while (a < shown.size()) {
            size_t b = shown.find('\n', a);
            if (b == std::string::npos) b = shown.size();
            std::string ln = shown.substr(a, b - a);
            std::string lower = ln;
            for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (lower.find("ancora") != std::string::npos) left += ln + "\n";
            a = b + 1;
        }
        say(left.empty() ? L"no ancora driver registered" : L"still registered: " + wide(left));
    } else {
        say(L"SteamVR not found, skipping driver unregister");
    }

    if (sp.config.empty()) { say(L"Steam config folder not found, skipping"); return 0; }
    std::wstring drv_cfg = sp.config + L"\\ancora";
    if (is_dir(drv_cfg)) {
        if (remove_tree(drv_cfg)) say(L"removed " + drv_cfg);
        else { say(L"cannot remove " + drv_cfg); ++fails; }
    }
    if (edit_json(sp.config + L"\\steamvr.vrsettings", backup_dir, vrsettings_strip) == Edit::Failed) ++fails;
    if (edit_json(sp.config + L"\\appconfig.json", backup_dir,
                  [](Json& j) { return appconfig_set(j, std::string()); }) == Edit::Failed) ++fails;
    for (auto& name : owned_sections()) {
        if (name == "driver_ancora") continue;
        std::wstring f = sp.config + L"\\vrappconfig\\" + wide(name) + L".vrappconfig";
        if (exists(f)) {
            if (DeleteFileW(f.c_str())) say(L"removed " + f);
            else { say(L"cannot remove " + f); ++fails; }
        }
    }
    return fails ? 1 : 0;
}

// action: "check", "register" or "unregister"; exit code for the process
inline int run(const std::wstring& action, const std::wstring& root, const std::wstring& backup_dir) {
    if (steamvr_running()) { say(L"SteamVR is running. Close it first."); return 1; }
    if (action == L"check") return 0;
    if (action == L"register") return do_register(root, backup_dir);
    return do_unregister(root, backup_dir);
}

} // namespace svr
