#include "sys_switch.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <sysclk/client/ipc.h>
}

namespace qs {

Services g_services;

namespace {

// Shared with the bt-autoconnect sysmodule (see its README): while this file
// exists it does not reconnect headphones on boot or wake.
constexpr const char* kAutoconnectDir = "sdmc:/config/bt-autoconnect";
constexpr const char* kPauseFlag = "sdmc:/config/bt-autoconnect/paused";
constexpr const char* kTargetFile = "sdmc:/config/bt-autoconnect/last_device.txt";
constexpr const char* kAutoconnectNsp = "sdmc:/atmosphere/contents/420000000000BA7C/exefs.nsp";
constexpr const char* kOverlayDir = "sdmc:/switch/.overlays";

// Boost targets: the fastest clocks Nintendo itself uses (sys-clk further caps
// the GPU in handheld mode).
constexpr uint32_t kBoostHz[SysClkModule_EnumMax] = {1785000000, 921600000, 1600000000};

bool Exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool AddrEq(const BtdrvAddress& a, const BtdrvAddress& b) { return memcmp(a.address, b.address, 6) == 0; }

std::string DeviceName(const BtmAudioDevice& d) {
    char name[sizeof(d.name) + 1] = {};
    memcpy(name, d.name, sizeof(d.name));
    return name;
}

// Last connected headphones as remembered by bt-autoconnect.
bool ReadTarget(BtdrvAddress& addr, std::string& name) {
    FILE* f = fopen(kTargetFile, "r");
    if (!f) return false;
    char line[0x120] = "";
    unsigned b[6];
    bool ok = fgets(line, sizeof line, f) &&
              sscanf(line, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
    if (ok) {
        for (int i = 0; i < 6; i++) addr.address[i] = u8(b[i]);
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            name = line;
        }
    }
    fclose(f);
    return ok;
}

void SetPaused(bool paused) {
    if (paused) {
        mkdir("sdmc:/config", 0777);
        mkdir(kAutoconnectDir, 0777);
        if (FILE* f = fopen(kPauseFlag, "w")) {
            fputs("set by quick-settings: headphones turned off\n", f);
            fclose(f);
        }
    } else {
        remove(kPauseFlag);
    }
}

int ConnectedDevices(BtmAudioDevice* out, int max) {
    s32 n = 0;
    if (R_FAILED(btmsysGetConnectedAudioDevices(out, max, &n))) return 0;
    return std::clamp<int>(n, 0, max);
}

void DisconnectAll() {
    BtmAudioDevice devs[4];
    int n = ConnectedDevices(devs, 4);
    for (int i = 0; i < n; i++) btmsysDisconnectAudioDevice(devs[i].addr);
}

// Name and version from the NACP inside an overlay (.ovl is an NRO).
bool ReadOverlayInfo(const std::string& path, OverlayEntry& o) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    u32 nroSize = 0;
    NroAssetHeader asset;
    if (fseek(f, sizeof(NroStart) + offsetof(NroHeader, size), SEEK_SET) == 0 && fread(&nroSize, 4, 1, f) == 1 &&
        fseek(f, nroSize, SEEK_SET) == 0 && fread(&asset, sizeof asset, 1, f) == 1 && asset.magic == NROASSETHEADER_MAGIC &&
        asset.nacp.size >= sizeof(NacpStruct)) {
        char name[0x201] = {}, version[0x11] = {};
        long base = long(nroSize + asset.nacp.offset);
        if (fseek(f, base, SEEK_SET) == 0 && fread(name, 1, 0x200, f) == 0x200 &&
            fseek(f, base + offsetof(NacpStruct, display_version), SEEK_SET) == 0 && fread(version, 1, 0x10, f) == 0x10) {
            o.name = name;
            o.version = version;
            ok = !o.name.empty();
        }
    }
    fclose(f);
    return ok;
}

}  // namespace

SwitchSystem::~SwitchSystem() {
    if (sysclk_) sysclkIpcExit();
}

void SwitchSystem::ConnectSysclk() {
    if (sysclk_) return;
    u64 now = armTicksToNs(armGetSystemTick());
    if (sysclkTry_ && now - sysclkTry_ < 3'000'000'000ULL) return;
    sysclkTry_ = now;
    // sm would block a GetService for a name nobody registered yet; ask first.
    WithSm([&] {
        if (sysclkIpcRunning() && R_SUCCEEDED(sysclkIpcInitialize())) sysclk_ = true;
        Log("sys-clk %s", sysclk_ ? "connected" : "not running");
    });
}

void SwitchSystem::RefreshClock(State& s) {
    if (g_services.time) {
        u64 ts = 0;
        TimeCalendarTime cal;
        TimeCalendarAdditionalInfo info;
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts)) &&
            R_SUCCEEDED(timeToCalendarTimeWithMyRule(ts, &cal, &info))) {
            s.hour = cal.hour;
            s.minute = cal.minute;
            s.day = cal.day;
            s.month = cal.month;
            s.weekday = int(info.wday);
        }
    }

    if (g_services.psm) {
        u32 pct = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) s.battery = int(pct);
        PsmChargerType charger = PsmChargerType_Unconnected;
        psmGetChargerType(&charger);
        s.pluggedIn = charger != PsmChargerType_Unconnected;
        bool enabled = true;
        s.chargeCtlReady = R_SUCCEEDED(psmIsBatteryChargingEnabled(&enabled));
        s.chargingEnabled = enabled;
        s.charging = s.pluggedIn && enabled && s.battery < 100;
    }
}

void SwitchSystem::Refresh(State& s) {
    RefreshClock(s);

    if (g_services.apm) {
        ApmPerformanceMode mode = ApmPerformanceMode_Normal;
        if (R_SUCCEEDED(apmGetPerformanceMode(&mode))) s.docked = mode == ApmPerformanceMode_Boost;
    }

    s.btReady = g_services.bt;
    s.autoconnect = Exists(kAutoconnectNsp);
    s.hpPaused = Exists(kPauseFlag);
    if (g_services.bt) {
        bool radio = true;
        if (R_SUCCEEDED(btmsysGetRadioOnOff(&radio))) s.radioOn = radio;

        BtmAudioDevice devs[4];
        int n = ConnectedDevices(devs, 4);
        s.hpConnected = n > 0;
        bool connecting = false;
        btmsysIsConnectingAudioDevice(&connecting);
        s.hpConnecting = connecting && !s.hpConnected;
        if (n > 0) {
            s.hpName = DeviceName(devs[0]);
        } else {
            BtdrvAddress addr;
            std::string name;
            if (ReadTarget(addr, name)) s.hpName = name;
        }
        for (AudioDevice& d : s.paired) {
            d.connected = false;
            for (int i = 0; i < n; i++)
                if (memcmp(devs[i].addr.address, d.addr, 6) == 0) d.connected = true;
        }
    }

    s.wifiReady = g_services.nifm;
    if (g_services.nifm) {
        bool on = false;
        nifmIsWirelessCommunicationEnabled(&on);
        s.wifiOn = on;
        NifmInternetConnectionType type;
        u32 strength = 0;
        NifmInternetConnectionStatus status;
        s.wifiOnline = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
                       status == NifmInternetConnectionStatus_Connected;
        s.ssid.clear();
        static NifmNetworkProfileData profile;
        if (s.wifiOnline && R_SUCCEEDED(nifmGetCurrentNetworkProfile(&profile))) {
            const auto& w = profile.wireless_setting_data;
            size_t n = std::min<size_t>(w.ssid_len, sizeof(w.ssid));
            if (const void* nul = memchr(w.ssid, 0, n)) n = size_t(static_cast<const char*>(nul) - w.ssid);
            s.ssid.assign(w.ssid, n);
        }
    }
    s.airplane = s.wifiReady && s.btReady && !s.wifiOn && !s.radioOn;

    ConnectSysclk();
    s.sysclkReady = false;
    if (sysclk_) {
        SysClkContext ctx;
        if (R_SUCCEEDED(sysclkIpcGetCurrentContext(&ctx))) {
            s.sysclkReady = true;
            s.cpuMHz = ctx.realFreqs[SysClkModule_CPU] / 1000000;
            s.gpuMHz = ctx.realFreqs[SysClkModule_GPU] / 1000000;
            s.memMHz = ctx.realFreqs[SysClkModule_MEM] / 1000000;
            s.socTemp = int(ctx.temps[SysClkThermalSensor_SOC] / 1000);
            s.pcbTemp = int(ctx.temps[SysClkThermalSensor_PCB] / 1000);
            s.powerMw = ctx.power[SysClkPowerSensor_Now];
            s.powerKnown = true;
            s.boost = ctx.overrideFreqs[SysClkModule_CPU] != 0;
            s.docked = ctx.profile == SysClkProfile_Docked;
        } else {
            sysclkIpcExit();
            sysclk_ = false;
        }
    }

    s.lblReady = g_services.lbl;
    if (g_services.lbl) {
        float b = s.brightness;
        if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&b))) s.brightness = b;
        bool a = false;
        if (R_SUCCEEDED(lblIsAutoBrightnessControlEnabled(&a))) s.autoBrightness = a;
    }
}

void SwitchSystem::ListDevices(State& s) {
    s.paired.clear();
    if (g_services.bt) {
        static BtmAudioDevice devs[10];
        s32 n = 0;
        if (R_SUCCEEDED(btmsysGetPairedAudioDevices(devs, 10, &n))) {
            for (int i = 0; i < std::min<int>(n, 10); i++) {
                AudioDevice d;
                memcpy(d.addr, devs[i].addr.address, 6);
                d.name = DeviceName(devs[i]);
                s.paired.push_back(d);
            }
        }
    }
    Refresh(s);  // marks the connected one
}

void SwitchSystem::ListOverlays(State& s) {
    s.overlays.clear();
    if (DIR* dir = opendir(kOverlayDir)) {
        while (dirent* e = readdir(dir)) {
            std::string file = e->d_name;
            if (file.size() < 5 || file.compare(file.size() - 4, 4, ".ovl") != 0 || file == "ovlmenu.ovl" ||
                file[0] == '.')
                continue;
            OverlayEntry o;
            o.path = std::string(kOverlayDir) + "/" + file;
            if (!ReadOverlayInfo(o.path, o)) o.name = file.substr(0, file.size() - 4);
            s.overlays.push_back(o);
        }
        closedir(dir);
        std::sort(s.overlays.begin(), s.overlays.end(),
                  [](const OverlayEntry& a, const OverlayEntry& b) { return a.name < b.name; });
    }
}

void SwitchSystem::SetHeadphones(State& s, bool on) {
    if (!g_services.bt) return;
    if (!on) {
        SetPaused(true);
        DisconnectAll();
        s.hpConnected = s.hpConnecting = false;
        s.hpPaused = true;
        return;
    }
    SetPaused(false);
    s.hpPaused = false;
    BtdrvAddress target = {};
    std::string name;
    bool have = ReadTarget(target, name);
    if (!have && !s.paired.empty()) {
        memcpy(target.address, s.paired[0].addr, 6);
        have = true;
    }
    Result rc = have ? btmsysConnectAudioDevice(target) : 0;
    Log("headphones on: target=%d rc=0x%X", have, rc);
    if (have && R_SUCCEEDED(rc)) s.hpConnecting = true;
}

void SwitchSystem::ToggleDevice(State& s, const AudioDevice& d) {
    if (!g_services.bt) return;
    BtdrvAddress addr;
    memcpy(addr.address, d.addr, 6);
    if (d.connected) {
        SetPaused(true);
        btmsysDisconnectAudioDevice(addr);
        s.hpPaused = true;
        return;
    }
    // One audio device at a time: drop the current one first.
    BtmAudioDevice devs[4];
    int n = ConnectedDevices(devs, 4);
    for (int i = 0; i < n; i++)
        if (!AddrEq(devs[i].addr, addr)) btmsysDisconnectAudioDevice(devs[i].addr);
    SetPaused(false);
    s.hpPaused = false;
    Result rc = btmsysConnectAudioDevice(addr);
    Log("connect %s rc=0x%X", d.name.c_str(), rc);
    if (R_SUCCEEDED(rc)) s.hpConnecting = true;
}

void SwitchSystem::SetWifi(State& s, bool on) {
    if (g_services.nifm && R_SUCCEEDED(nifmSetWirelessCommunicationEnabled(on))) s.wifiOn = on;
}

// Airplane mode: Wi-Fi and the Bluetooth radio off, both also stored in
// system settings like the HOME menu's own flight mode toggle does.
void SwitchSystem::SetAirplane(State& s, bool on) {
    if (!g_services.nifm || !g_services.bt) return;
    Result wifi = nifmSetWirelessCommunicationEnabled(!on);
    Result radio = on ? btmsysDisableRadio() : btmsysEnableRadio();
    if (g_services.setsys) {
        setsysSetWirelessLanEnableFlag(!on);
        setsysSetBluetoothEnableFlag(!on);
    }
    Log("airplane %s: wifi rc=0x%X radio rc=0x%X", on ? "on" : "off", wifi, radio);
    if (R_SUCCEEDED(wifi)) s.wifiOn = !on;
    if (R_SUCCEEDED(radio)) s.radioOn = !on;
    s.airplane = !s.wifiOn && !s.radioOn;
}

void SwitchSystem::SetBoost(State& s, bool on) {
    if (!sysclk_) return;
    for (int m = 0; m < SysClkModule_EnumMax; m++) {
        auto module = SysClkModule(m);
        if (!on) {
            sysclkIpcRemoveOverride(module);
            continue;
        }
        u32 freqs[SYSCLK_FREQ_LIST_MAX];
        u32 count = 0;
        u32 best = 0;
        if (R_SUCCEEDED(sysclkIpcGetFreqList(module, freqs, SYSCLK_FREQ_LIST_MAX, &count)))
            for (u32 i = 0; i < count; i++)
                if (freqs[i] <= kBoostHz[m]) best = std::max(best, freqs[i]);
        u32 hz = best ? best : kBoostHz[m];
        Result rc = sysclkIpcSetOverride(module, hz);
        Log("boost %s -> %u Hz rc=0x%X", sysclkFormatModule(module, false), hz, rc);
    }
    s.boost = on;
}

void SwitchSystem::SetChargingEnabled(State& s, bool on) {
    if (!g_services.psm) return;
    if (R_SUCCEEDED(on ? psmEnableBatteryCharging() : psmDisableBatteryCharging())) s.chargingEnabled = on;
}

void SwitchSystem::SetBrightness(State& s, float v, bool commit) {
    if (!g_services.lbl) return;
    if (R_SUCCEEDED(lblSetCurrentBrightnessSetting(v))) s.brightness = v;
    if (commit) lblSaveCurrentSetting();
}

void SwitchSystem::SetAutoBrightness(State& s, bool on) {
    if (!g_services.lbl) return;
    if (R_SUCCEEDED(on ? lblEnableAutoBrightnessControl() : lblDisableAutoBrightnessControl())) s.autoBrightness = on;
    lblSaveCurrentSetting();
}

static void Shutdown(bool reboot) {
    WithSm([&] {
        if (R_SUCCEEDED(spsmInitialize())) {
            spsmShutdown(reboot);
            spsmExit();
        }
    });
}

void SwitchSystem::Reboot() { Shutdown(true); }
void SwitchSystem::PowerOff() { Shutdown(false); }

void SwitchSystem::LaunchOverlay(const OverlayEntry& o) {
    // Same hand-off as Tesla Menu: the loader runs this .ovl next and the
    // overlay opens right away because of --skipCombo.
    const char* file = strrchr(o.path.c_str(), '/');
    std::string args = std::string(file ? file + 1 : o.path.c_str()) + " --skipCombo";
    envSetNextLoad(o.path.c_str(), args.c_str());
}

}  // namespace qs
