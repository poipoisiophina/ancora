// probe.cpp
// Prints identity of every connected controller SteamVR currently reports, so
// the binding file can target the exact controller_type.

#include <openvr.h>
#include <cstdio>
using namespace vr;

static void prop(IVRSystem* s, TrackedDeviceIndex_t i, ETrackedDeviceProperty p, const char* label) {
    char buf[256]{}; ETrackedPropertyError e;
    s->GetStringTrackedDeviceProperty(i, p, buf, sizeof(buf), &e);
    std::printf("    %-20s = %s\n", label, e == TrackedProp_Success ? buf : "(n/a)");
}

int main() {
    EVRInitError e = VRInitError_None;
    IVRSystem* s = VR_Init(&e, VRApplication_Background);
    if (e != VRInitError_None) { std::printf("VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(e)); return 1; }
    for (TrackedDeviceIndex_t i = 0; i < k_unMaxTrackedDeviceCount; ++i) {
        if (s->GetTrackedDeviceClass(i) != TrackedDeviceClass_Controller) continue;
        ETrackedControllerRole role = s->GetControllerRoleForTrackedDeviceIndex(i);
        std::printf("device %u  role=%s  connected=%d\n", i,
                    role == TrackedControllerRole_LeftHand ? "L" :
                    role == TrackedControllerRole_RightHand ? "R" : "?",
                    s->IsTrackedDeviceConnected(i));
        prop(s, i, Prop_ControllerType_String,       "ControllerType");
        prop(s, i, Prop_ModelNumber_String,          "ModelNumber");
        prop(s, i, Prop_RenderModelName_String,      "RenderModel");
        prop(s, i, Prop_RegisteredDeviceType_String, "RegisteredType");
        prop(s, i, Prop_ManufacturerName_String,     "Manufacturer");
    }
    VR_Shutdown();
    return 0;
}
