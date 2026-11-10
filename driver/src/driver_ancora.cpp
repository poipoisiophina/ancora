// driver_ancora.cpp
// Fixed SteamVR driver: a loader with no input logic of its own. It patches the
// IVRDriverInput vtables inside vrserver, records every component other drivers
// create, and forwards create and update events to logic\ancora_logic.dll.
// That dll is hot reloaded when its file changes: the loader loads a private
// copy, so the original can be rebuilt in place, and replays the recorded
// components to each new logic instance. Runs inside vrserver only.
// Limits: only interface versions whose first four slots are known are patched,
// and components a driver created before ancora loaded are never seen.

#include <openvr_driver.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ancora_host_api.h"

using namespace vr;

namespace {

// IVRDriverInput vtable slots: CreateBooleanComponent, UpdateBooleanComponent,
// CreateScalarComponent, UpdateScalarComponent. The same in versions 001 to 004
// (openvr_driver.h history) and in 005, which SteamVR's own vrlink driver uses.
// A later version may differ, so it is logged and left alone.
enum { kCreateBool = 0, kUpdateBool = 1, kCreateScalar = 2, kUpdateScalar = 3, kSlots = 4 };
const int kFirstVersion = 1, kLastKnownVersion = 5, kProbeTo = 20;

using CreateBoolFn   = EVRInputError (*)(void*, PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*);
using UpdateBoolFn   = EVRInputError (*)(void*, VRInputComponentHandle_t, bool, double);
using CreateScalarFn = EVRInputError (*)(void*, PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*,
                                         EVRScalarType, EVRScalarUnits);
using UpdateScalarFn = EVRInputError (*)(void*, VRInputComponentHandle_t, float, double);

struct Patched {
    void**         vtable = nullptr;
    void*          original[kSlots] = {};
    CreateBoolFn   create_bool   = nullptr;
    UpdateBoolFn   update_bool   = nullptr;
    CreateScalarFn create_scalar = nullptr;
    UpdateScalarFn update_scalar = nullptr;
};

const int kMaxTables = 20;
Patched g_tables[kMaxTables];
int g_ntables = 0;

struct Component {
    int kind;
    PropertyContainerHandle_t container;
    std::string name;
    VRInputComponentHandle_t handle;
    void* iface;
};

SRWLOCK g_lock = SRWLOCK_INIT;      // shared in hooks, exclusive for reload
std::vector<Component> g_components;
const AncoraLogic* g_logic = nullptr;
HMODULE g_logic_mod = nullptr;
int g_logic_slot = -1;              // live copy in use, 0 or 1, -1 = none
std::wstring g_dir, g_logic_path;
std::string g_dir_utf8;
FILETIME g_logic_time{};
int g_generation = 0;
int g_retries = 0;                  // failed loads of the current file, retried a few times
const int kMaxRetries = 10;

void log(const std::string& s) { VRDriverLog()->Log(("ancora: " + s).c_str()); }

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// the patched table behind an interface object, nullptr when it is not one of ours
Patched* table_of(void* self) {
    if (!self) return nullptr;
    void** vt = *(void***)self;
    for (int i = 0; i < g_ntables; ++i) if (g_tables[i].vtable == vt) return &g_tables[i];
    static bool warned = false;
    if (!warned) { warned = true; log("call through an input table ancora did not patch, ignored"); }
    return nullptr;
}

// ---- host api handed to the logic ---------------------------------------
void host_log(const char* m) { log(m); }
int32_t host_get_int(uint64_t c, int32_t p) {
    ETrackedPropertyError e;
    return VRProperties()->GetInt32Property(c, (ETrackedDeviceProperty)p, &e);
}
uint32_t host_get_string(uint64_t c, int32_t p, char* buf, uint32_t n) {
    ETrackedPropertyError e;
    return VRProperties()->GetStringProperty(c, (ETrackedDeviceProperty)p, buf, n, &e);
}
void host_send_scalar(void* iface, uint64_t h, float v, double dt) {
    if (Patched* t = table_of(iface)) t->update_scalar(iface, h, v, dt);
}
void host_send_bool(void* iface, uint64_t h, bool v, double dt) {
    if (Patched* t = table_of(iface)) t->update_bool(iface, h, v, dt);
}

AncoraHost g_host{ANCORA_API_VERSION, nullptr, host_log, host_get_int, host_get_string,
                   host_send_scalar, host_send_bool};

// ---- logic loading ---------------------------------------------------------
// g_lock held exclusive
void unload_logic() {
    if (g_logic && g_logic->shutdown) g_logic->shutdown();
    g_logic = nullptr;
    if (g_logic_mod) FreeLibrary(g_logic_mod);
    g_logic_mod = nullptr;
    g_logic_slot = -1;
}

// true when loaded, false to try again later
bool reload_logic() {
    // load a private copy so the build can overwrite the original at any time; the
    // copy goes to the slot the running logic does not use, so it is never locked
    int slot = g_logic_slot == 0 ? 1 : 0;
    std::wstring live = g_dir + L"\\logic\\ancora_logic_live" + std::to_wstring(slot) + L".dll";
    if (!CopyFileW(g_logic_path.c_str(), live.c_str(), FALSE)) {
        log("copy logic failed " + std::to_string(GetLastError()));
        return false;
    }
    HMODULE mod = LoadLibraryExW(live.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto init = mod ? (AncoraLogicInitFn)GetProcAddress(mod, "ancora_logic_init") : nullptr;
    const AncoraLogic* logic = init ? init(&g_host) : nullptr;
    if (!logic || logic->version != ANCORA_API_VERSION || !logic->start) {
        log("logic load failed, keeping previous");
        if (mod) FreeLibrary(mod);
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    unload_logic();          // the old logic stops writing before the new one starts
    g_logic = logic;
    g_logic_mod = mod;
    g_logic_slot = slot;
    ++g_generation;
    g_logic->start((uint32_t)g_generation);
    for (const Component& c : g_components)
        if (g_logic->on_create) g_logic->on_create(c.kind, c.container, c.name.c_str(), c.handle, c.iface);
    ReleaseSRWLockExclusive(&g_lock);
    log("logic loaded, generation " + std::to_string(g_generation) + ", replayed " +
        std::to_string(g_components.size()) + " component(s)");
    return true;
}

void check_logic() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_logic_path.c_str(), GetFileExInfoStandard, &fa)) return;
    bool changed = CompareFileTime(&fa.ftLastWriteTime, &g_logic_time) != 0;
    if (changed) g_retries = 0;
    else if (g_retries == 0 || g_retries >= kMaxRetries) return;   // loaded, or gave up on this file
    g_logic_time = fa.ftLastWriteTime;
    // a failed load (the file still being written, say) is tried again on the next checks
    g_retries = reload_logic() ? 0 : g_retries + 1;
    if (g_retries >= kMaxRetries) log("logic load gave up until the file changes again");
}

// ---- hooks -----------------------------------------------------------------
void record(int kind, PropertyContainerHandle_t c, const char* name, VRInputComponentHandle_t h, void* self) {
    AcquireSRWLockExclusive(&g_lock);
    g_components.push_back({kind, c, name, h, self});
    if (g_logic && g_logic->on_create) g_logic->on_create(kind, c, name, h, self);
    ReleaseSRWLockExclusive(&g_lock);
}

EVRInputError hk_create_bool(void* self, PropertyContainerHandle_t c, const char* name, VRInputComponentHandle_t* h) {
    Patched* t = table_of(self);
    if (!t) return VRInputError_InvalidHandle;
    EVRInputError r = t->create_bool(self, c, name, h);
    if (r == VRInputError_None && h && name) record(ANCORA_BOOL, c, name, *h, self);
    return r;
}

EVRInputError hk_create_scalar(void* self, PropertyContainerHandle_t c, const char* name, VRInputComponentHandle_t* h,
                               EVRScalarType ty, EVRScalarUnits un) {
    Patched* t = table_of(self);
    if (!t) return VRInputError_InvalidHandle;
    EVRInputError r = t->create_scalar(self, c, name, h, ty, un);
    if (r == VRInputError_None && h && name) record(ANCORA_SCALAR, c, name, *h, self);
    return r;
}

EVRInputError hk_update_bool(void* self, VRInputComponentHandle_t h, bool v, double dt) {
    Patched* t = table_of(self);
    if (!t) return VRInputError_InvalidHandle;
    AcquireSRWLockShared(&g_lock);
    if (g_logic && g_logic->on_bool) g_logic->on_bool(self, h, &v, dt);
    ReleaseSRWLockShared(&g_lock);
    return t->update_bool(self, h, v, dt);
}

EVRInputError hk_update_scalar(void* self, VRInputComponentHandle_t h, float v, double dt) {
    Patched* t = table_of(self);
    if (!t) return VRInputError_InvalidHandle;
    AcquireSRWLockShared(&g_lock);
    if (g_logic && g_logic->on_scalar) g_logic->on_scalar(self, h, &v, dt);
    ReleaseSRWLockShared(&g_lock);
    return t->update_scalar(self, h, v, dt);
}

HMODULE module_of(const void* p) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)p, &m);
    return m;
}

bool write_slots(void** vt, void* const values[kSlots]) {
    DWORD old;
    if (!VirtualProtect(vt, sizeof(void*) * kSlots, PAGE_READWRITE, &old)) return false;
    for (int s = 0; s < kSlots; ++s) vt[s] = values[s];
    VirtualProtect(vt, sizeof(void*) * kSlots, old, &old);
    FlushInstructionCache(GetCurrentProcess(), vt, sizeof(void*) * kSlots);
    return true;
}

void patch(void* iface, const char* version) {
    if (!iface || g_ntables >= kMaxTables) return;
    void** vt = *(void***)iface;
    for (int i = 0; i < g_ntables; ++i) if (g_tables[i].vtable == vt) return;
    // the four slots must be code in the same module as the table itself, else
    // the layout is not what ancora expects and nothing is touched
    HMODULE home = module_of(vt);
    for (int s = 0; s < kSlots; ++s)
        if (!home || module_of(vt[s]) != home) { log(std::string(version) + " table looks different, not patched"); return; }
    Patched& p = g_tables[g_ntables];
    p.vtable = vt;
    for (int s = 0; s < kSlots; ++s) p.original[s] = vt[s];
    p.create_bool   = (CreateBoolFn)vt[kCreateBool];
    p.update_bool   = (UpdateBoolFn)vt[kUpdateBool];
    p.create_scalar = (CreateScalarFn)vt[kCreateScalar];
    p.update_scalar = (UpdateScalarFn)vt[kUpdateScalar];
    ++g_ntables;   // register before swapping so hooks always find their table
    void* hooks[kSlots] = {(void*)&hk_create_bool, (void*)&hk_update_bool, (void*)&hk_create_scalar,
                           (void*)&hk_update_scalar};
    if (!write_slots(vt, hooks)) {
        --g_ntables;
        log("VirtualProtect failed " + std::to_string(GetLastError()));
    }
}

// puts the original calls back, so nothing calls into ancora once it is unloaded
void unpatch_all() {
    for (int i = 0; i < g_ntables; ++i) write_slots(g_tables[i].vtable, g_tables[i].original);
}

} // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name, int* rc);

namespace {

class Provider : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        // every version the runtime offers: drivers built against other SDKs use
        // other versions, each with its own table
        for (int n = kFirstVersion; n <= kProbeTo; ++n) {
            char v[32];
            std::snprintf(v, sizeof(v), "IVRDriverInput_%03d", n);
            EVRInitError e = VRInitError_None;
            void* iface = ctx->GetGenericInterface(v, &e);
            if (!iface) continue;
            if (n > kLastKnownVersion) { log(std::string("found ") + v + ", newer than ancora knows, not patched"); continue; }
            log(std::string("found ") + v);
            patch(iface, v);
        }
        log("patched " + std::to_string(g_ntables) + " input interface table(s)");

        // driver root is two levels above bin\win64
        wchar_t path[MAX_PATH]{};
        HMODULE self = module_of((const void*)&HmdDriverFactory);
        GetModuleFileNameW(self, path, MAX_PATH);
        g_dir = path;
        for (int up = 0; up < 3; ++up) g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/"));
        g_dir_utf8 = utf8(g_dir);
        g_host.driver_dir = g_dir_utf8.c_str();
        g_logic_path = g_dir + L"\\logic\\ancora_logic.dll";
        check_logic();
        return VRInitError_None;
    }
    void Cleanup() override {
        AcquireSRWLockExclusive(&g_lock);
        unpatch_all();
        unload_logic();
        ReleaseSRWLockExclusive(&g_lock);
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    void RunFrame() override {
        uint64_t now = GetTickCount64();
        if (now - last_check_ >= 1000) { last_check_ = now; check_logic(); }
        AcquireSRWLockShared(&g_lock);
        if (g_logic && g_logic->on_frame) g_logic->on_frame();
        ReleaseSRWLockShared(&g_lock);
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    uint64_t last_check_ = 0;
};

Provider g_provider;

} // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name, int* rc) {
    if (std::strcmp(IServerTrackedDeviceProvider_Version, name) == 0) return &g_provider;
    if (rc) *rc = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
