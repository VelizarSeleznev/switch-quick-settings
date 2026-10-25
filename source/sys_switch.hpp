// System implementation on the Switch (libnx services).
#pragma once

#include <switch.h>

#include "qs.hpp"

namespace qs {

// Services opened in __appInit; false when one could not be opened.
struct Services {
    bool time = false, psm = false, apm = false, bt = false, nifm = false, lbl = false, setsys = false;
};
extern Services g_services;

// Appends a line to sdmc:/config/quick-settings/log.txt (kept under 32 KB).
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Runs fn with an sm session open. Like libtesla, the overlay opens sm only
// around service lookups instead of holding a session.
template <typename F>
void WithSm(F&& fn) {
    if (R_FAILED(smInitialize())) return;
    fn();
    smExit();
}

class SwitchSystem final : public System {
public:
    ~SwitchSystem() override;
    void Refresh(State& s) override;
    void RefreshClock(State& s) override;
    void ListDevices(State& s) override;
    void ListOverlays(State& s) override;
    void SetHeadphones(State& s, bool on) override;
    void ToggleDevice(State& s, const AudioDevice& d) override;
    void SetWifi(State& s, bool on) override;
    void SetAirplane(State& s, bool on) override;
    void SetBoost(State& s, bool on) override;
    void SetChargingEnabled(State& s, bool on) override;
    void SetBrightness(State& s, float v, bool commit) override;
    void SetAutoBrightness(State& s, bool on) override;
    void Reboot() override;
    void PowerOff() override;
    void LaunchOverlay(const OverlayEntry& o) override;

private:
    void ConnectSysclk();

    bool sysclk_ = false;
    uint64_t sysclkTry_ = 0;
};

}  // namespace qs
