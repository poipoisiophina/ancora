// ancora_host_api.h
// C ABI between the fixed loader (driver_ancora.dll) and the hot swappable
// logic (ancora_logic.dll). The loader owns the hooks and forwards events; the
// logic owns all behaviour. Bump ANCORA_API_VERSION on any layout change.

#pragma once
#include <cstdint>

#define ANCORA_API_VERSION 1

enum AncoraComponentKind { ANCORA_BOOL = 0, ANCORA_SCALAR = 1 };

struct AncoraHost {
    uint32_t version;
    const char* driver_dir;        // driver root, no trailing slash
    void (*log)(const char* msg);
    // property reads on a component's container
    int32_t (*get_int_prop)(uint64_t container, int32_t prop);
    uint32_t (*get_string_prop)(uint64_t container, int32_t prop, char* buf, uint32_t size);
    // send a value through the original (unhooked) update calls
    void (*send_scalar)(void* iface, uint64_t handle, float value, double time_offset);
    void (*send_bool)(void* iface, uint64_t handle, bool value, double time_offset);
};

struct AncoraLogic {
    uint32_t version;
    // a component was created (also replayed for existing ones after a reload)
    void (*on_create)(int kind, uint64_t container, const char* name, uint64_t handle, void* iface);
    // value updates; the logic may rewrite *value before it is forwarded
    void (*on_bool)(void* iface, uint64_t handle, bool* value, double time_offset);
    void (*on_scalar)(void* iface, uint64_t handle, float* value, double time_offset);
    void (*on_frame)();            // once per server frame
    void (*shutdown)();            // before unload; must stop touching the host
};

// exported by the logic dll as "ancora_logic_init"
typedef const AncoraLogic* (*AncoraLogicInitFn)(const AncoraHost* host);
