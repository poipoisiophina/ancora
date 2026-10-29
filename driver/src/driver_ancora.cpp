// driver_ancora.cpp
// Fixed SteamVR driver: a loader with no input logic of its own. It patches the
// IVRDriverInput vtables inside vrserver, records every component other drivers
// create, and forwards create and update events to logic\ancora_logic.dll.
// That dll is hot reloaded when its file changes: the loader loads a private
// copy, so the original can be rebuilt in place, and replays the recorded
// components to each new logic instance. Runs inside vrserver only.

#include <openvr_driver.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ancora_host_api.h"

using namespace vr;

namespace {

// IVRDriverInput vtable slots, identical in versions 001 to 004
enum { kCreateBool = 0, kUpdateBool = 1, kCreateScalar = 2, kUpdateScalar = 3 };

using CreateBoolFn   = EVRInputError (*)(void*, PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*);
using UpdateBoolFn   = EVRInputError (*)(void*, VRInputComponentHandle_t, bool, double);
using CreateScalarFn = EVRInputError (*)(void*, PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*,
                                         EVRScalarType, EVRScalarUnits);
using UpdateScalarFn = EVRInputError (*)(void*, VRInputComponentHandle_t, float, double);

struct Patched {
    void**         vtable = nullptr;
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
std::string g_dir, g_logic_path, g_live_path;
FILETIME g_logic_time{};
int g_generation = 0;

void log(const std::string& s) { VRDriverLog()->Log(("ancora: " + s).c_str()); }

Patched* table_of(void* self) {
    void** vt = *(void***)self;
    for (int i = 0; i < g_ntables; ++i) if (g_tables[i].vtable == vt) return &g_tables[i];
    return &g_tables[0];
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
void host_send_scalar(void* iface, uint64_t h, float v, double dt) { table_of(iface)->update_scalar(iface, h, v, dt); }
void host_send_bool(void* iface, uint64_t h, bool v, double dt) { table_of(iface)->update_bool(iface, h, v, dt); }

AncoraHost g_host{ANCORA_API_VERSION, nullptr, host_log, host_get_int, host_get_string,
                   host_send_scalar, host_send_bool};

// ---- logic loading ---------------------------------------------------------
void unload_logic() {
    if (g_logic && g_logic->shutdown) g_logic->shutdown();
    g_logic = nullptr;
    if (g_logic_mod) FreeLibrary(g_logic_mod);
    g_logic_mod = nullptr;
}

void reload_logic() {
    // load a private copy so the build can overwrite the original at any time
    std::string live = g_dir + "\\logic\\ancora_logic_live" + std::to_string(++g_generation % 2) + ".dll";
    if (!CopyFileA(g_logic_path.c_str(), live.c_str(), FALSE)) {
        log("copy logic failed " + std::to_string(GetLastError()));
        return;
    }
    HMODULE mod = LoadLibraryA(live.c_str());
    auto init = mod ? (AncoraLogicInitFn)GetProcAddress(mod, "ancora_logic_init") : nullptr;
    const AncoraLogic* logic = init ? init(&g_host) : nullptr;
    if (!logic || logic->version != ANCORA_API_VERSION) {
        log("logic load failed, keeping previous");
        if (mod) FreeLibrary(mod);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    unload_logic();
    g_logic = logic;
    g_logic_mod = mod;
    for (const Component& c : g_components)
        if (g_logic->on_create) g_logic->on_create(c.kind, c.container, c.name.c_str(), c.handle, c.iface);
    ReleaseSRWLockExclusive(&g_lock);
    log("logic loaded, generation " + std::to_string(g_generation) + ", replayed " +
        std::to_string(g_components.size()) + " component(s)");
}

void check_logic() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_logic_path.c_str(), GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_logic_time) == 0) return;
    g_logic_time = fa.ftLastWriteTime;
    reload_logic();
}

// ---- hooks -----------------------------------------------------------------
void record(int kind, PropertyContainerHandle_t c, const char* name, VRInputComponentHandle_t h, void* self) {
    AcquireSRWLockExclusive(&g_lock);
    g_components.push_back({kind, c, name, h, self});
    if (g_logic && g_logic->on_create) g_logic->on_create(kind, c, name, h, self);
    ReleaseSRWLockExclusive(&g_lock);
}

EVRInputError hk_create_bool(void* self, PropertyContainerHandle_t c, const char* name, VRInputComponentHandle_t* h) {
    EVRInputError r = table_of(self)->create_bool(self, c, name, h);
    if (r == VRInputError_None && h && name) record(ANCORA_BOOL, c, name, *h, self);
    return r;
}

EVRInputError hk_create_scalar(void* self, PropertyContainerHandle_t c, const char* name, VRInputComponentHandle_t* h,
                               EVRScalarType ty, EVRScalarUnits un) {
    EVRInputError r = table_of(self)->create_scalar(self, c, name, h, ty, un);
    if (r == VRInputError_None && h && name) record(ANCORA_SCALAR, c, name, *h, self);
    return r;
}

EVRInputError hk_update_bool(void* self, VRInputComponentHandle_t h, bool v, double dt) {
    AcquireSRWLockShared(&g_lock);
    if (g_logic && g_logic->on_bool) g_logic->on_bool(self, h, &v, dt);
    ReleaseSRWLockShared(&g_lock);
    return table_of(self)->update_bool(self, h, v, dt);
}

EVRInputError hk_update_scalar(void* self, VRInputComponentHandle_t h, float v, double dt) {
    AcquireSRWLockShared(&g_lock);
    if (g_logic && g_logic->on_scalar) g_logic->on_scalar(self, h, &v, dt);
    ReleaseSRWLockShared(&g_lock);
    return table_of(self)->update_scalar(self, h, v, dt);
}

void patch(void* iface) {
    if (!iface || g_ntables >= kMaxTables) return;
    void** vt = *(void***)iface;
    for (int i = 0; i < g_ntables; ++i) if (g_tables[i].vtable == vt) return;
    Patched& p = g_tables[g_ntables];
    p.vtable = vt;
    p.create_bool   = (CreateBoolFn)vt[kCreateBool];
    p.update_bool   = (UpdateBoolFn)vt[kUpdateBool];
    p.create_scalar = (CreateScalarFn)vt[kCreateScalar];
    p.update_scalar = (UpdateScalarFn)vt[kUpdateScalar];
    DWORD old;
    if (!VirtualProtect(vt, sizeof(void*) * 4, PAGE_READWRITE, &old)) {
        log("VirtualProtect failed " + std::to_string(GetLastError()));
        return;
    }
    ++g_ntables;   // register before swapping so hooks always find their table
    vt[kCreateBool]   = (void*)&hk_create_bool;
    vt[kUpdateBool]   = (void*)&hk_update_bool;
    vt[kCreateScalar] = (void*)&hk_create_scalar;
    vt[kUpdateScalar] = (void*)&hk_update_scalar;
    VirtualProtect(vt, sizeof(void*) * 4, old, &old);
}

} // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name, int* rc);

namespace {

class Provider : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        // probe every version the runtime offers; drivers built against newer
        // SDKs (vrlink uses 005) get their own table. Slots 0..3 are stable.
        for (int n = 1; n <= 20; ++n) {
            char v[32];
            std::snprintf(v, sizeof(v), "IVRDriverInput_%03d", n);
            EVRInitError e = VRInitError_None;
            void* iface = ctx->GetGenericInterface(v, &e);
            if (iface) { log(std::string("found ") + v); patch(iface); }
        }
        log("patched " + std::to_string(g_ntables) + " input interface table(s)");

        // driver root is two levels above bin\win64
        char path[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&HmdDriverFactory, &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        g_dir = path;
        for (int up = 0; up < 3; ++up) g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
        g_host.driver_dir = g_dir.c_str();
        g_logic_path = g_dir + "\\logic\\ancora_logic.dll";
        check_logic();
        return VRInitError_None;
    }
    void Cleanup() override {
        AcquireSRWLockExclusive(&g_lock);
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
