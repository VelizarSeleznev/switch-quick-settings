// Overlay runtime: an nx-ovlloader overlay (ovlmenu.ovl) that owns a vi layer
// at the left or right screen edge, watches for an edge swipe or the button
// combo in a background thread, and runs the Ui while the panel is open.
//
// Layer, focus and input handling follow libtesla (WerWolv, GPL-2.0).
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <strings.h>
#include <sys/stat.h>

#include "gfx.hpp"
#include "sys_switch.hpp"
#include "ui.hpp"

using namespace qs;

static_assert(key::A == HidNpadButton_A && key::Down == HidNpadButton_Down && key::StickR == HidNpadButton_StickR &&
              key::StickLDown == HidNpadButton_StickLDown);

extern "C" u64 __nx_vi_layer_id;  // libnx: id of the layer nwindow uses

namespace {

constexpr u64 kSec = 1'000'000'000ULL;
constexpr int kFbW = Ui::kLayerW, kFbH = Ui::kLayerH;
// vi positions layers in 1920x1080 space; the 448x720 framebuffer is scaled to 672x1080.
constexpr float kLayerW1080 = 1080.f * kFbW / kFbH;
constexpr const char* kStateDir = "sdmc:/config/quick-settings";
constexpr const char* kSideFile = "sdmc:/config/quick-settings/side";
constexpr const char* kLogFile = "sdmc:/config/quick-settings/log.txt";

double Now() { return double(armTicksToNs(armGetSystemTick())) / kSec; }

void Must(Result rc) {
    if (R_FAILED(rc)) diagAbortWithResult(rc);
}

}  // namespace

void qs::Log(const char* fmt, ...) {
    mkdir("sdmc:/config", 0777);
    mkdir(kStateDir, 0777);
    struct stat st;
    FILE* f = fopen(kLogFile, stat(kLogFile, &st) == 0 && st.st_size > 32 * 1024 ? "w" : "a");
    if (!f) return;
    fprintf(f, "[%9.3f] ", Now());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

namespace {

// ------------------------------------------------------------ focus

Result EnableAppletInput(bool enable, u64 aruid) {
    const struct {
        u8 permit;
        u64 aruid;
    } in = {enable, aruid};
    return serviceDispatchIn(hidsysGetServiceSession(), 503, in);
}

// Takes input away from the game and the system applets (or gives it back).
void RequestForeground(bool enabled) {
    u64 pid = 0;
    for (u64 programId = 0x0100000000001000UL; programId < 0x0100000000001020UL; programId++) {
        pid = 0;
        pmdmntGetProcessId(&pid, programId);
        if (pid) EnableAppletInput(!enabled, pid);
    }
    pid = 0;
    pmdmntGetApplicationProcessId(&pid);
    EnableAppletInput(!enabled, pid);
    EnableAppletInput(true, 0);
}

// ------------------------------------------------------------ layer

Result AddToLayerStack(ViLayer* layer, ViLayerStack stack) {
    const struct {
        u32 stack;
        u64 layerId;
    } in = {stack, layer->layer_id};
    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

struct Screen {
    ViDisplay display;
    ViLayer layer;
    NWindow window;
    Framebuffer fb;
    Event vsync;

    void Init() {
        WithSm([&] {
            Must(viInitialize(ViServiceType_Manager));
            Must(viOpenDefaultDisplay(&display));
            Must(viGetDisplayVsyncEvent(&display, &vsync));
            Must(viCreateManagedLayer(&display, ViLayerFlags(0), 0, &__nx_vi_layer_id));
            Must(viCreateLayer(&display, &layer));
            Must(viSetLayerScalingMode(&layer, ViScalingMode_FitToLayer));
            s32 z = 0;
            if (R_SUCCEEDED(viGetZOrderCountMax(&display, &z)) && z > 0) viSetLayerZ(&layer, z);
            // Visible everywhere, including screenshots and recordings (like Tesla overlays).
            for (ViLayerStack s : {ViLayerStack_Default, ViLayerStack_Screenshot, ViLayerStack_Recording,
                                   ViLayerStack_Arbitrary, ViLayerStack_LastFrame, ViLayerStack_Null,
                                   ViLayerStack_ApplicationForDebug, ViLayerStack_Lcd})
                AddToLayerStack(&layer, s);
            Must(viSetLayerSize(&layer, u64(kLayerW1080), 1080));
            Must(viSetLayerPosition(&layer, 0, 0));
            Must(nwindowCreateFromLayer(&window, &layer));
            Must(framebufferCreate(&fb, &window, kFbW, kFbH, PIXEL_FORMAT_RGBA_4444, 2));
        });
    }

    void Exit() {
        framebufferClose(&fb);
        nwindowClose(&window);
        viDestroyManagedLayer(&layer);
        viCloseDisplay(&display);
        eventClose(&vsync);
        viExit();
    }

    void Place(Side side) { viSetLayerPosition(&layer, side == Side::Left ? 0 : 1920 - kLayerW1080, 0); }

    void Present(Canvas& cv, Ui& ui) {
        void* buf = framebufferBegin(&fb, nullptr);
        cv.Bind(static_cast<uint16_t*>(buf), true);
        ui.Present(cv);
        eventWait(&vsync, UINT64_MAX);
        framebufferEnd(&fb);
    }
};

// ------------------------------------------------------------ input thread

struct Shared {
    std::mutex mutex;
    // Written by the input thread.
    u64 keysDown = 0, keysHeld = 0;
    Touch touch;                // latest sample; when released it keeps the last position
    // Press and release transitions since the main thread last looked, so a tap
    // shorter than one frame is not lost.
    Touch touchEvents[16];
    int touchEventCount = 0;
    // Requests to the main thread.
    UEvent openEvent;
    Side openSide = Side::Left;
    bool openTracking = false;
    std::atomic<bool> open{false};          // panel currently shown
    std::atomic<bool> closeRequest{false};  // combo pressed while open
    std::atomic<bool> hideNow{false};       // HOME / power: hide without animation
    std::atomic<bool> running{true};
    u64 combo = HidNpadButton_L | HidNpadButton_Down | HidNpadButton_StickR;
};

Shared g;

u64 ParseCombo(const char* s) {
    static const struct {
        const char* name;
        u64 key;
    } kNames[] = {{"A", HidNpadButton_A},         {"B", HidNpadButton_B},          {"X", HidNpadButton_X},
                  {"Y", HidNpadButton_Y},         {"LS", HidNpadButton_StickL},    {"LSTICK", HidNpadButton_StickL},
                  {"RS", HidNpadButton_StickR},   {"RSTICK", HidNpadButton_StickR}, {"L", HidNpadButton_L},
                  {"R", HidNpadButton_R},         {"ZL", HidNpadButton_ZL},        {"ZR", HidNpadButton_ZR},
                  {"PLUS", HidNpadButton_Plus},   {"MINUS", HidNpadButton_Minus},  {"DLEFT", HidNpadButton_Left},
                  {"DUP", HidNpadButton_Up},      {"DRIGHT", HidNpadButton_Right}, {"DDOWN", HidNpadButton_Down}};
    u64 keys = 0;
    std::string token;
    for (const char* p = s;; p++) {
        if (*p == '+' || *p == 0 || *p == '\r' || *p == '\n' || *p == ' ') {
            for (auto& k : kNames)
                if (strcasecmp(token.c_str(), k.name) == 0) keys |= k.key;
            token.clear();
            if (*p != '+' && *p != ' ') break;
        } else {
            token += *p;
        }
    }
    return keys;
}

// The same combo as other Tesla overlays: sdmc:/config/tesla/config.ini, [tesla] key_combo.
void LoadCombo() {
    FILE* f = fopen("sdmc:/config/tesla/config.ini", "r");
    if (!f) return;
    char line[128];
    bool inTesla = false;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '[') inTesla = strncmp(line, "[tesla]", 7) == 0;
        else if (inTesla && strncmp(line, "key_combo", 9) == 0) {
            const char* v = strchr(line, '=');
            if (v) {
                while (*++v == ' ') {}
                if (u64 keys = ParseCombo(v)) g.combo = keys;
            }
        }
    }
    fclose(f);
}

// Edge swipe recognizer, used while the panel is hidden.
struct EdgeSwipe {
    static constexpr float kEdge = 18;      // touch must start this close to a screen edge
    static constexpr float kTravel = 34;    // inward travel that counts as a swipe
    static constexpr double kMaxTime = 0.6;
    bool active = false;
    Side side = Side::Left;
    float x0 = 0, y0 = 0;
    double t0 = 0;

    // Returns true when a swipe is recognized.
    bool Feed(bool down, float x, float y, bool wasDown) {
        if (!down) {
            active = false;
            return false;
        }
        if (!wasDown) {
            active = x <= kEdge || x >= Ui::kScreenW - kEdge;
            side = x <= kEdge ? Side::Left : Side::Right;
            x0 = x;
            y0 = y;
            t0 = Now();
            return false;
        }
        if (!active) return false;
        float inward = side == Side::Left ? x - x0 : x0 - x;
        float dy = y - y0;
        if (Now() - t0 > kMaxTime || std::abs(dy) > 60) {
            active = false;
            return false;
        }
        if (inward >= kTravel && std::abs(dy) < inward) {
            active = false;
            return true;
        }
        return false;
    }
};

void InputThread(void*) {
    Event home = {}, power = {};
    hidsysAcquireHomeButtonEventHandle(&home, false);
    hidsysAcquireSleepButtonEventHandle(&power, false);
    eventClear(&home);
    eventClear(&power);

    padConfigureInput(8, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt);
    PadState pad;
    padInitializeAny(&pad);
    hidInitializeTouchScreen();
    padUpdate(&pad);

    EdgeSwipe swipe;
    bool wasDown = false;
    float lastX = 0, lastY = 0;
    while (g.running) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad), held = padGetButtons(&pad);
        HidTouchScreenState ts = {};
        bool touching = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
        if (touching) {
            lastX = float(ts.touches[0].x);
            lastY = float(ts.touches[0].y);
        }
        {
            std::scoped_lock lock(g.mutex);
            g.keysDown |= down;
            g.keysHeld = held;
            g.touch = {touching, lastX, lastY};
            if (touching != wasDown && g.touchEventCount < 16) g.touchEvents[g.touchEventCount++] = g.touch;
        }

        if ((held & g.combo) == g.combo && (down & g.combo)) {
            if (g.open) g.closeRequest = true;
            else {
                g.openTracking = false;
                ueventSignal(&g.openEvent);
            }
        } else if (!g.open && swipe.Feed(touching, lastX, lastY, wasDown)) {
            g.openSide = swipe.side;
            g.openTracking = true;
            ueventSignal(&g.openEvent);
        }
        wasDown = touching;

        s32 idx = -1;
        Result rc = waitMulti(&idx, 8'000'000, waiterForEvent(&home), waiterForEvent(&power));
        if (R_SUCCEEDED(rc)) {
            eventClear(idx == 0 ? &home : &power);
            if (g.open) g.hideNow = true;
        }
    }
    eventClose(&home);
    eventClose(&power);
}

Side LoadSide() {
    FILE* f = fopen(kSideFile, "r");
    if (!f) return Side::Left;
    int c = fgetc(f);
    fclose(f);
    return c == 'R' ? Side::Right : Side::Left;
}

void SaveSide(Side side) {
    mkdir("sdmc:/config", 0777);
    mkdir(kStateDir, 0777);
    if (FILE* f = fopen(kSideFile, "w")) {
        fputc(side == Side::Right ? 'R' : 'L', f);
        fclose(f);
    }
}

bool SystemIsRussian() {
    u64 code = 0;
    SetLanguage lang;
    bool ru = false;
    WithSm([&] {
        if (R_SUCCEEDED(setInitialize())) {
            ru = R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang)) &&
                 lang == SetLanguage_RU;
            setExit();
        }
    });
    return ru;
}

}  // namespace

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_nv_transfermem_size = 0x40000;
ViLayerFlags __nx_vi_stray_layer_flags = ViLayerFlags(0);

void __appInit(void) {
    WithSm([] {
        Must(fsInitialize());
        fsdevMountSdmc();
        Must(hidInitialize());
        Must(pmdmntInitialize());
        Must(hidsysInitialize());
        g_services.time = R_SUCCEEDED(timeInitialize());
        g_services.psm = R_SUCCEEDED(psmInitialize());
        g_services.apm = R_SUCCEEDED(apmInitialize());
        g_services.bt = R_SUCCEEDED(btmsysInitialize());
        g_services.nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_Admin));
        g_services.lbl = R_SUCCEEDED(lblInitialize());
        g_services.setsys = R_SUCCEEDED(setsysInitialize());
    });
}

void __appExit(void) {
    if (g_services.setsys) setsysExit();
    if (g_services.lbl) lblExit();
    if (g_services.nifm) nifmExit();
    if (g_services.bt) btmsysExit();
    if (g_services.apm) apmExit();
    if (g_services.psm) psmExit();
    if (g_services.time) timeExit();
    hidsysExit();
    pmdmntExit();
    hidExit();
    fsdevUnmountAll();
    fsExit();
}
}

int main(int argc, char** argv) {
    bool openNow = false;
    for (int i = 0; i < argc; i++)
        if (strcasecmp(argv[i], "--skipCombo") == 0) openNow = true;

    LoadCombo();
    ueventCreate(&g.openEvent, true);

    Screen screen;
    screen.Init();
    Canvas canvas(kFbW, kFbH);
    SwitchSystem sys;
    Ui ui(sys, SystemIsRussian());
    // Fill the state once so the first open has something to show right away.
    sys.Refresh(ui.state());
    sys.ListOverlays(ui.state());
    Log("start: HOS %u.%u.%u, skipCombo=%d, combo=0x%llx, services time=%d psm=%d apm=%d bt=%d nifm=%d lbl=%d "
        "setsys=%d",
        HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()), openNow,
        (unsigned long long)g.combo, g_services.time, g_services.psm, g_services.apm, g_services.bt, g_services.nifm,
        g_services.lbl, g_services.setsys);

    Thread input;
    threadCreate(&input, InputThread, nullptr, nullptr, 0x4000, 0x2c, -2);
    threadStart(&input);

    if (openNow) {
        g.openSide = LoadSide();
        g.openTracking = false;
        ueventSignal(&g.openEvent);
    }

    bool quit = false;
    while (!quit) {
        waitSingle(waiterForUEvent(&g.openEvent), UINT64_MAX);
        Side side = g.openSide;
        bool tracking = g.openTracking;
        g.closeRequest = false;
        g.hideNow = false;
        g.open = true;
        {
            std::scoped_lock lock(g.mutex);
            g.keysDown = 0;
            g.touchEventCount = 0;
        }
        RequestForeground(true);
        screen.Place(side);
        ui.Open(side, tracking, Now());
        Log("open %s%s", side == Side::Left ? "left" : "right", tracking ? " (edge swipe)" : "");

        while (true) {
            Input in;
            Touch events[16];
            int eventCount;
            {
                std::scoped_lock lock(g.mutex);
                in.down = g.keysDown;
                in.held = g.keysHeld;
                in.touch = g.touch;
                eventCount = g.touchEventCount;
                std::copy(g.touchEvents, g.touchEvents + eventCount, events);
                g.touchEventCount = 0;
                g.keysDown = 0;
            }
            in.now = Now();
            if (g.hideNow) {
                ui.HideNow();
                break;
            }
            if (g.closeRequest.exchange(false)) ui.Close(in.now);
            // Every press/release first (a quick tap is both within one frame), then the current position.
            for (int i = 0; i < eventCount; i++) {
                ui.Update({i == 0 ? in.down : 0, in.held, events[i], in.now});
                if (i == 0) in.down = 0;
            }
            bool alive = ui.Update(in);
            if (ui.NeedsPresent()) screen.Present(canvas, ui);
            else svcSleepThread(kSec / 120);
            if (!alive) break;
        }
        g.open = false;
        screen.Present(canvas, ui);  // ui is closed: draws an empty frame

        OverlayEntry launch;
        Ui::Request request = ui.TakeRequest(&launch);
        Log("closed%s, request %d", g.hideNow ? " by HOME/power" : "", int(request));
        switch (request) {
            case Ui::Request::Launch:
                SaveSide(ui.side());
                sys.LaunchOverlay(launch);
                quit = true;
                break;
            case Ui::Request::Reboot:
                sys.Reboot();
                break;
            case Ui::Request::PowerOff:
                sys.PowerOff();
                break;
            case Ui::Request::None:
                break;
        }
        RequestForeground(false);
        ueventClear(&g.openEvent);
    }

    g.running = false;
    threadWaitForExit(&input);
    threadClose(&input);
    screen.Exit();
    return 0;
}
