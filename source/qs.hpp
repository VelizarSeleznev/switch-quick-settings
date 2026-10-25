// Platform-independent model of the quick settings panel: what the UI shows
// (State) and what it can ask the console to do (System).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qs {

// Button bits, identical to libnx HidNpadButton (checked in main.cpp).
namespace key {
constexpr uint64_t A = 1ull << 0, B = 1ull << 1, X = 1ull << 2, Y = 1ull << 3;
constexpr uint64_t StickL = 1ull << 4, StickR = 1ull << 5, L = 1ull << 6, R = 1ull << 7;
constexpr uint64_t ZL = 1ull << 8, ZR = 1ull << 9, Plus = 1ull << 10, Minus = 1ull << 11;
constexpr uint64_t Left = 1ull << 12, Up = 1ull << 13, Right = 1ull << 14, Down = 1ull << 15;
constexpr uint64_t StickLLeft = 1ull << 16, StickLUp = 1ull << 17, StickLRight = 1ull << 18, StickLDown = 1ull << 19;
constexpr uint64_t AnyLeft = Left | StickLLeft, AnyRight = Right | StickLRight;
constexpr uint64_t AnyUp = Up | StickLUp, AnyDown = Down | StickLDown;
}  // namespace key

struct AudioDevice {
    uint8_t addr[6] = {};
    std::string name;
    bool connected = false;
};

struct OverlayEntry {
    std::string path, name, version;
};

struct State {
    // Clock and battery.
    int hour = 0, minute = 0, weekday = 0, day = 1, month = 1;  // weekday 0 = Sunday, month 1..12
    int battery = 100;
    bool charging = false;      // battery is gaining charge
    bool pluggedIn = false;     // charger or dock connected
    bool docked = false;

    // Bluetooth headphones.
    bool btReady = false;
    bool radioOn = true;        // Bluetooth radio (off in airplane mode)
    bool hpConnected = false, hpConnecting = false;
    std::string hpName;         // connected device, or the remembered target
    bool hpPaused = false;      // bt-autoconnect told not to reconnect
    bool autoconnect = false;   // bt-autoconnect sysmodule installed
    std::vector<AudioDevice> paired;

    // Wi-Fi and airplane mode (Wi-Fi and Bluetooth radio both off).
    bool wifiReady = false, wifiOn = false, wifiOnline = false;
    std::string ssid;
    bool airplane = false;

    // sys-clk.
    bool sysclkReady = false, boost = false;
    uint32_t cpuMHz = 0, gpuMHz = 0, memMHz = 0;
    int socTemp = -1, pcbTemp = -1;   // degrees C
    int powerMw = 0;                  // battery power now (negative while charging)
    bool powerKnown = false;

    // Charging control (psm).
    bool chargeCtlReady = false, chargingEnabled = true;

    // Backlight (lbl).
    bool lblReady = false;
    float brightness = 0.5f;
    bool autoBrightness = false;

    std::vector<OverlayEntry> overlays;
};

class System {
public:
    virtual ~System() = default;
    // Status poll (connections, clocks, sensors, brightness). Called every
    // half second while the panel is open and nothing is moving.
    virtual void Refresh(State& s) = 0;
    // Clock and battery only: cheap, called as the panel opens.
    virtual void RefreshClock(State& s) = 0;
    // Lists, read when their page opens: paired audio devices, other overlays.
    virtual void ListDevices(State& s) = 0;
    virtual void ListOverlays(State& s) = 0;

    virtual void SetHeadphones(State& s, bool on) = 0;     // on: connect target; off: disconnect + pause autoconnect
    virtual void ToggleDevice(State& s, const AudioDevice& d) = 0;
    virtual void SetWifi(State& s, bool on) = 0;
    virtual void SetAirplane(State& s, bool on) = 0;
    virtual void SetBoost(State& s, bool on) = 0;
    virtual void SetChargingEnabled(State& s, bool on) = 0;
    virtual void SetBrightness(State& s, float v, bool commit) = 0;
    virtual void SetAutoBrightness(State& s, bool on) = 0;
    virtual void Reboot() = 0;
    virtual void PowerOff() = 0;
    virtual void LaunchOverlay(const OverlayEntry& o) = 0;
};

}  // namespace qs
