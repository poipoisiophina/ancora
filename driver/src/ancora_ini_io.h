// ancora_ini_io.h
// Reading and changing ancora.ini from the logic dll (inside vrserver) and the
// panel, which both write it: the panel when a setting changes, the driver when
// it saves learned edge reach. Every change is read, edit, write under one named
// mutex, written to a temp file and swapped in, so neither side ever reads half
// a file or loses the other's change. Wide paths only, so any user name works.

#pragma once
#include <windows.h>
#include <string>
#include "ancora_config.h"

namespace ancfg {

#define ANCORA_INI_MUTEX L"Local\\ancora_ini"

// whole file as bytes; false when it does not exist or cannot be read
inline bool read_file(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = INVALID_HANDLE_VALUE;
    // the other side may be swapping the file in right now: retry briefly
    for (int tries = 0; tries < 20; ++tries) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return false;
        Sleep(5);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > (1 << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)size.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    out.resize(rd);
    return ok != FALSE;
}

// temp file in the same folder, then an atomic replace
inline bool write_file(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    for (int tries = 0; tries < 20; ++tries) {   // a reader without delete sharing (an editor) may hold it
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(10);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// holds the named mutex for the life of the object (at most 2 s wait); ok() is
// false when it could not be had, and then nothing may be written
struct IniLock {
    HANDLE m = nullptr;
    bool held = false;
    IniLock() {
        m = CreateMutexW(nullptr, FALSE, ANCORA_INI_MUTEX);
        if (!m) return;
        DWORD w = WaitForSingleObject(m, 2000);
        held = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;   // abandoned: the owner died, the lock is ours
    }
    ~IniLock() {
        if (held) ReleaseMutex(m);
        if (m) CloseHandle(m);
    }
    bool ok() const { return held; }
    IniLock(const IniLock&) = delete;
    IniLock& operator=(const IniLock&) = delete;
};

// read, edit, write as one step. edit gets the current text ("" when the file is
// missing) and returns the new text; returning the same text writes nothing.
template <class F>
bool update(const std::wstring& path, F edit) {
    IniLock lock;
    if (!lock.ok()) return false;   // never write without the lock: the other side may be mid change
    std::string text;
    read_file(path, text);
    std::string next = edit(text);
    if (next == text) return true;
    return write_file(path, next);
}

inline bool set_key(const std::wstring& path, const char* key, const std::string& value) {
    return update(path, [&](const std::string& t) { return set(t, key, value); });
}

// <hand>_edge and <hand>_edge_base together, as one change
inline bool set_edge(const std::wstring& path, int hand, const EdgeReach& edge, const EdgeReach& base) {
    std::string k = kHandKey[hand];
    return update(path, [&](const std::string& t) {
        return set(set(t, (k + "_edge").c_str(), edge_format(edge)), (k + "_edge_base").c_str(), edge_format(base));
    });
}

} // namespace ancfg
