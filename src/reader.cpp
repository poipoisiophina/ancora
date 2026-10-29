// reader.cpp
// SteamVR background reader using the action based input API, IVRInput.
// Connects to SteamVR as a background application, binds the thumbstick
// position and its touch for both hands, and prints them. Touches no game.
//
// Purpose: confirm the thumbstick value and its touch bit are readable from
// outside the game through the streamed controllers. This decides whether the
// virtual controller driver is feasible.

#include <openvr.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

using namespace vr;

static VRActionHandle_t stickL, stickR, touchL, touchR;
static VRActionSetHandle_t setAncora;

static void read_hand(const char* tag, VRActionHandle_t stick, VRActionHandle_t touch) {
    InputAnalogActionData_t a{};
    InputDigitalActionData_t d{};
    EVRInputError ea = VRInput()->GetAnalogActionData(stick, &a, sizeof(a), k_ulInvalidInputValueHandle);
    EVRInputError ed = VRInput()->GetDigitalActionData(touch, &d, sizeof(d), k_ulInvalidInputValueHandle);
    std::printf("  %s stick active=%d (%+.3f,%+.3f)  touch active=%d state=%d  [ea=%d ed=%d]\n",
                tag, a.bActive, a.x, a.y, d.bActive, d.bState, (int)ea, (int)ed);
}

int main(int argc, char** argv) {
    EVRInitError e = VRInitError_None;
    // overlay type: global action set priority is only honored for overlay apps
    VR_Init(&e, VRApplication_Overlay);
    if (e != VRInitError_None) {
        std::printf("VR_Init failed: %s\n", VR_GetVRInitErrorAsEnglishDescription(e));
        return 1;
    }

    // manifest and bindings sit next to the exe, whatever the current folder is
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* slash = std::strrchr(dir, '\\')) slash[1] = 0;

    // register and identify this process so SteamVR applies the default binding
    char vrman[MAX_PATH];
    std::snprintf(vrman, sizeof(vrman), "%sancora.vrmanifest", dir);
    EVRApplicationError ae = VRApplications()->AddApplicationManifest(vrman, true);
    EVRApplicationError ie = VRApplications()->IdentifyApplication(GetCurrentProcessId(), "ancora.reader");
    char appkey[256]{};
    VRApplications()->GetApplicationKeyByProcessId(GetCurrentProcessId(), appkey, sizeof(appkey));
    std::printf("AddApplicationManifest('%s') -> %d ; Identify -> %d ; appKey='%s'\n",
                vrman, (int)ae, (int)ie, appkey);

    char manifest[MAX_PATH];
    if (argc > 1) GetFullPathNameA(argv[1], MAX_PATH, manifest, nullptr);
    else std::snprintf(manifest, sizeof(manifest), "%sbindings\\action_manifest.json", dir);
    EVRInputError me = VRInput()->SetActionManifestPath(manifest);
    std::printf("SetActionManifestPath('%s') -> %d\n", manifest, (int)me);

    VRInput()->GetActionSetHandle("/actions/ancora", &setAncora);
    VRInput()->GetActionHandle("/actions/ancora/in/StickL", &stickL);
    VRInput()->GetActionHandle("/actions/ancora/in/StickR", &stickR);
    VRInput()->GetActionHandle("/actions/ancora/in/TouchL", &touchL);
    VRInput()->GetActionHandle("/actions/ancora/in/TouchR", &touchR);

    std::printf("Reading for ~20s. Move each stick, and lift your thumb off and "
                "back on, so axis and touch change.\n\n");

    VRActiveActionSet_t active{};
    active.ulActionSet = setAncora;
    active.ulRestrictedToDevice = k_ulInvalidInputValueHandle;
    // receive input regardless of which scene app has focus
    active.nPriority = k_nActionSetOverlayGlobalPriorityMin;

    // warm up, then report which origins (if any) the binding attached to
    for (int w = 0; w < 10; ++w) { VRInput()->UpdateActionState(&active, sizeof(active), 1); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    VRActionHandle_t acts[4] = {stickL, stickR, touchL, touchR};
    const char* an[4] = {"StickL", "StickR", "TouchL", "TouchR"};
    for (int k = 0; k < 4; ++k) {
        VRInputValueHandle_t org[8]{};
        EVRInputError oe = VRInput()->GetActionOrigins(setAncora, acts[k], org, 8);
        int cnt = 0; for (int j = 0; j < 8; ++j) if (org[j] != k_ulInvalidInputValueHandle) ++cnt;
        std::printf("origins[%s] err=%d count=%d", an[k], (int)oe, cnt);
        for (int j = 0; j < 8; ++j) if (org[j] != k_ulInvalidInputValueHandle) {
            char nm[256]{}; VRInput()->GetOriginLocalizedName(org[j], nm, sizeof(nm), VRInputString_All);
            std::printf(" | %s", nm);
        }
        std::printf("\n");
    }
    std::printf("\n");

    // focus diagnostics: whether SteamVR is withholding input from this process
    std::printf("IsInputAvailable=%d ShouldApplicationPause=%d IsSteamVRDrawingControllers=%d\n",
                (int)VRSystem()->IsInputAvailable(), (int)VRSystem()->ShouldApplicationPause(),
                (int)VRSystem()->IsSteamVRDrawingControllers());
    VRInputValueHandle_t hL = 0, hR = 0;
    VRInput()->GetInputSourceHandle("/user/hand/left", &hL);
    VRInput()->GetInputSourceHandle("/user/hand/right", &hR);
    std::printf("source handles L=%llu R=%llu\n\n", (unsigned long long)hL, (unsigned long long)hR);

    for (int i = 0; i < 100; ++i) {
        VRInput()->UpdateActionState(&active, sizeof(active), 1);
        std::printf("[t=%4dms]\n", i * 200);
        read_hand("L", stickL, touchL);
        read_hand("R", stickR, touchR);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    VR_Shutdown();
    return 0;
}
