// ancora_host_api.h
// C ABI between the fixed loader (driver_ancora.dll) and the hot swappable
// logic (ancora_logic.dll). The loader owns the hooks and forwards events; the
// logic owns all behaviour. Bump ANCORA_API_VERSION on any layout or meaning change.
// Version 2: driver_dir is UTF-8; init only checks and prepares, start() begins
// the work (shared memory, config) once the previous logic has shut down, so two
// logic instances never write the panel's shared state at the same time.

#pragma once
#include <cstdint>

#define ANCORA_API_VERSION 2

enum AncoraComponentKind { ANCORA_BOOL = 0, ANCORA_SCALAR = 1 };

struct AncoraHost {
    uint32_t version;
    const char* driver_dir;        // driver root, UTF-8, no trailing slash
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
    // begin: called once, after the previous logic's shutdown, before any other call
    void (*start)(uint32_t generation);
    // a component was created (also replayed for existing ones after a reload)
    void (*on_create)(int kind, uint64_t container, const char* name, uint64_t handle, void* iface);
    // value updates; the logic may rewrite *value before it is forwarded
    void (*on_bool)(void* iface, uint64_t handle, bool* value, double time_offset);
    void (*on_scalar)(void* iface, uint64_t handle, float* value, double time_offset);
    void (*on_frame)();            // once per server frame
    void (*shutdown)();            // before unload; must stop touching the host
};

// exported by the logic dll as "ancora_logic_init"; must not touch shared state
typedef const AncoraLogic* (*AncoraLogicInitFn)(const AncoraHost* host);
